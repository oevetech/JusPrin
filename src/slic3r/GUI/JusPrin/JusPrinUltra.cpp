#include <GL/glew.h>   // precisa vir antes de qualquer cabeçalho que inclua gl.h

#include "JusPrinUltra.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <stdexcept>

#include <boost/beast/core/detail/base64.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <wx/glcanvas.h>
#include <wx/image.h>
#include <wx/mstream.h>
#include <wx/stdpaths.h>

#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/calib.hpp"

#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "slic3r/Utils/FixModelByWin10.hpp"

using nlohmann::json;

namespace Slic3r { namespace GUI {

namespace {

[[noreturn]] void fail(const std::string& msg) { throw std::runtime_error(msg); }

Plater* plater() { return wxGetApp().plater(); }
Model&  model() { return plater()->model(); }

size_t object_index(const json& p, const char* key = "index")
{
    const int idx = p.value(key, -1);
    if (idx < 0 || idx >= int(model().objects.size()))
        fail("Índice de objeto inválido: " + std::to_string(idx) + ". Use read_project para ver os índices.");
    return size_t(idx);
}

// Converte um valor JSON para o texto que ConfigBase::set_deserialize espera
// (mesmas regras da página oficial: lista -> primeiro item, booleano -> 1/0).
std::string config_value_string(const json& v)
{
    if (v.is_array())
        return v.empty() ? std::string() : config_value_string(v.front());
    if (v.is_boolean())
        return v.get<bool>() ? "1" : "0";
    if (v.is_string())
        return v.get<std::string>();
    return v.dump();
}

template<class Config>
json apply_settings(Config& config, const json& settings)
{
    if (!settings.is_object())
        fail("settings precisa ser um objeto chave: valor.");
    json result = json::object();
    ConfigSubstitutionContext ctx(ForwardCompatibilitySubstitutionRule::Enable);
    for (const auto& [key, value] : settings.items()) {
        try {
            config.set_deserialize(key, config_value_string(value), ctx);
            result[key] = "ok";
        } catch (const std::exception& e) {
            result[key] = std::string("erro: ") + e.what();
        }
    }
    return result;
}

// Depois de mexer direto no Model: recria a lista de objetos, reatribui as
// instâncias às mesas e manda o fatiador reprocessar.
void refresh_after_model_change(const std::vector<size_t>& changed, bool instances_changed = false)
{
    if (instances_changed)
        wxGetApp().obj_list()->reload_all_plates(true);
    else
        wxGetApp().obj_list()->update_after_undo_redo();
    if (!changed.empty())
        plater()->changed_objects(changed);
    plater()->update(false, true);
}

Vec3d xyz(const json& j, const Vec3d& fallback)
{
    return { j.value("x", fallback.x()), j.value("y", fallback.y()), j.value("z", fallback.z()) };
}

json vec_json(const Vec3d& v) { return { {"x", v.x()}, {"y", v.y()}, {"z", v.z()} }; }

Preset::Type preset_type(const std::string& type)
{
    if (type == "print") return Preset::TYPE_PRINT;
    if (type == "filament") return Preset::TYPE_FILAMENT;
    if (type == "printer") return Preset::TYPE_PRINTER;
    fail("Tipo de perfil inválido: " + type);
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string timestamp()
{
    char buf[32];
    std::time_t t = std::time(nullptr);
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", std::localtime(&t));
    return buf;
}

} // namespace

const std::map<std::string, JusPrinUltra::Handler>& JusPrinUltra::handlers()
{
    static const std::map<std::string, Handler> map = {
        {"jp_get_slice_stats", &JusPrinUltra::GetSliceStats},
        {"jp_get_layer_stats", &JusPrinUltra::GetLayerStats},
        {"jp_transform_objects", &JusPrinUltra::TransformObjects},
        {"jp_set_instances", &JusPrinUltra::SetInstances},
        {"jp_delete_objects", &JusPrinUltra::DeleteObjects},
        {"jp_cut_object", &JusPrinUltra::CutObject},
        {"jp_set_object_config", &JusPrinUltra::SetObjectConfig},
        {"jp_add_height_range", &JusPrinUltra::AddHeightRange},
        {"jp_add_modifier_volume", &JusPrinUltra::AddModifierVolume},
        {"jp_add_layer_gcode", &JusPrinUltra::AddLayerGcode},
        {"jp_adaptive_layer_height", &JusPrinUltra::AdaptiveLayerHeight},
        {"jp_plate_ops", &JusPrinUltra::PlateOps},
        {"jp_import_files", &JusPrinUltra::ImportFiles},
        {"jp_manage_preset", &JusPrinUltra::ManagePreset},
        {"jp_apply_printer_config", &JusPrinUltra::ApplyPrinterConfig},
        {"jp_run_calibration", &JusPrinUltra::RunCalibration},
        {"jp_save_project", &JusPrinUltra::SaveProject},
        {"jp_send_to_printer", &JusPrinUltra::SendToPrinter},
        {"jp_check_model", &JusPrinUltra::CheckModel},
        {"jp_repair_model", &JusPrinUltra::RepairModel},
        {"jp_render_preview", &JusPrinUltra::RenderPreview},
    };
    return map;
}

// ---------- Resultado do fatiamento ----------

json JusPrinUltra::GetSliceStats(const json&)
{
    json plates = json::array();
    PartPlateList& list = plater()->get_partplate_list();
    for (int i = 0; i < list.get_plate_count(); ++i) {
        PartPlate* plate = list.get_plate(i);
        json pj = { {"plate_index", i}, {"sliced", plate->is_slice_result_valid()} };
        if (plate->is_slice_result_valid()) {
            if (const Print* print = plate->fff_print()) {
                const PrintStatistics& ps = print->print_statistics();
                pj["filament_mm"] = ps.total_used_filament;
                pj["filament_g"] = ps.total_weight;
                pj["cost"] = ps.total_cost;
                pj["filament_changes"] = ps.total_toolchanges;
                pj["estimated_time_text"] = ps.estimated_normal_print_time;
            }
            if (const GCodeProcessorResult* r = plate->get_slice_result()) {
                const auto& mode = r->print_statistics.modes[0];
                pj["time_s"] = mode.time;
                pj["prepare_time_s"] = mode.prepare_time;
                pj["layer_count"] = mode.layers_times.size();
                json roles = json::object();
                for (const auto& [role, t] : mode.roles_times)
                    roles[ExtrusionEntity::role_to_string(role)] = t;
                pj["time_by_role_s"] = roles;
            }
        }
        plates.push_back(pj);
    }
    return { {"plates", plates} };
}

json JusPrinUltra::GetLayerStats(const json& p)
{
    PartPlateList& list = plater()->get_partplate_list();
    const int idx = p.value("plate_index", list.get_curr_plate_index());
    PartPlate* plate = list.get_plate(idx);
    if (!plate || !plate->is_slice_result_valid() || !plate->get_slice_result())
        fail("A mesa ainda não foi fatiada. Use slice antes.");
    const auto& times = plate->get_slice_result()->print_statistics.modes[0].layers_times;
    return { {"plate_index", idx}, {"layer_count", times.size()}, {"layer_times_s", times} };
}

// ---------- Objetos ----------

json JusPrinUltra::TransformObjects(const json& p)
{
    const json& objects = p.value("objects", json::array());
    if (!objects.is_array() || objects.empty())
        fail("objects precisa ser uma lista.");

    plater()->take_snapshot("JusBot: transformar objetos");
    std::vector<size_t> changed;
    json result = json::array();
    for (const json& o : objects) {
        const size_t idx = object_index(o);
        ModelObject* obj = model().objects[idx];
        for (ModelInstance* inst : obj->instances) {
            if (o.contains("scale_percent"))
                inst->set_scaling_factor(xyz(o["scale_percent"], inst->get_scaling_factor() * 100.) / 100.);
            if (o.contains("rotate_deg"))
                inst->set_rotation(xyz(o["rotate_deg"], inst->get_rotation() * 180. / PI) * PI / 180.);
            if (o.contains("move_to")) {
                const Vec3d off = inst->get_offset();
                inst->set_offset(xyz(o["move_to"], off));
            }
            if (o.contains("mirror")) {
                Vec3d m = inst->get_mirror();
                const std::string axis = o["mirror"].get<std::string>();
                const int a = axis == "x" ? 0 : axis == "y" ? 1 : 2;
                m[a] = -m[a];
                inst->set_mirror(m);
            }
        }
        obj->invalidate_bounding_box();
        obj->ensure_on_bed();
        changed.push_back(idx);
        const BoundingBoxf3 bb = obj->instance_bounding_box(0);
        result.push_back({ {"index", idx}, {"size_mm", vec_json(bb.size())}, {"center", vec_json(bb.center())} });
    }
    refresh_after_model_change(changed);
    return { {"objects", result} };
}

json JusPrinUltra::SetInstances(const json& p)
{
    const size_t idx = object_index(p);
    const int count = p.value("count", 1);
    if (count < 1 || count > 100)
        fail("count precisa estar entre 1 e 100.");

    plater()->take_snapshot("JusBot: cópias");
    ModelObject* obj = model().objects[idx];
    while (int(obj->instances.size()) < count) {
        ModelInstance* last = obj->instances.back();
        ModelInstance* added = obj->add_instance(*last);
        const BoundingBoxf3 bb = obj->instance_bounding_box(obj->instances.size() - 2);
        added->set_offset(last->get_offset() + Vec3d(bb.size().x() + 5., 0., 0.));
    }
    while (int(obj->instances.size()) > count)
        obj->delete_instance(obj->instances.size() - 1);
    obj->invalidate_bounding_box();
    refresh_after_model_change({ idx }, true);
    return { {"index", idx}, {"copies", obj->instances.size()},
             {"tip", "Use arrange para distribuir as cópias na mesa."} };
}

json JusPrinUltra::DeleteObjects(const json& p)
{
    std::vector<int> indices = p.value("indices", std::vector<int>{});
    if (indices.empty())
        fail("indices vazio.");
    for (int i : indices)
        if (i < 0 || i >= int(model().objects.size()))
            fail("Índice inválido: " + std::to_string(i));
    std::sort(indices.rbegin(), indices.rend());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());

    plater()->take_snapshot("JusBot: apagar objetos");
    for (int i : indices)
        plater()->delete_object_from_model(size_t(i));
    return { {"deleted", indices}, {"remaining", model().objects.size()} };
}

json JusPrinUltra::CutObject(const json& p)
{
    const size_t idx = object_index(p);
    const double z = p.value("z_mm", 0.0);
    ModelObject* obj = model().objects[idx];
    const BoundingBoxf3 bb = obj->instance_bounding_box(0);
    if (z <= bb.min.z() || z >= bb.max.z())
        fail("z_mm fora da peça (altura vai de " + std::to_string(bb.min.z()) + " a " + std::to_string(bb.max.z()) + " mm).");

    const std::string keep = p.value("keep", std::string("both"));
    ModelObjectCutAttributes attrs = ModelObjectCutAttribute::InvalidateCutInfo;
    if (keep != "lower") attrs = attrs | ModelObjectCutAttribute::KeepUpper;
    if (keep != "upper") attrs = attrs | ModelObjectCutAttribute::KeepLower;
    if (p.value("flip_upper", false)) attrs = attrs | ModelObjectCutAttribute::FlipUpper;

    // Mesma lógica de Plater::cut_horizontal (privado)
    plater()->take_snapshot("JusBot: cortar objeto");
    const Vec3d offset = obj->instances[0]->get_offset();
    Cut cut(obj, 0, Geometry::translation_transform(z * Vec3d::UnitZ() - offset), attrs);
    const ModelObjectPtrs new_objects = cut.perform_with_plane();
    plater()->apply_cut_object_to_model(idx, new_objects);
    return { {"cut_at_mm", z}, {"objects_now", model().objects.size()} };
}

json JusPrinUltra::SetObjectConfig(const json& p)
{
    const size_t idx = object_index(p);
    ModelObject* obj = model().objects[idx];
    plater()->take_snapshot("JusBot: configuração do objeto");
    json result;
    if (p.contains("part_index")) {
        const int part = p["part_index"].get<int>();
        if (part < 0 || part >= int(obj->volumes.size()))
            fail("part_index inválido.");
        result = apply_settings(obj->volumes[part]->config, p.value("settings", json::object()));
    } else {
        result = apply_settings(obj->config, p.value("settings", json::object()));
    }
    refresh_after_model_change({ idx });
    return { {"index", idx}, {"settings", result} };
}

json JusPrinUltra::AddHeightRange(const json& p)
{
    const size_t idx = object_index(p);
    const double z_min = p.value("z_min", 0.0), z_max = p.value("z_max", 0.0);
    if (z_max <= z_min)
        fail("z_max precisa ser maior que z_min.");
    ModelObject* obj = model().objects[idx];

    plater()->take_snapshot("JusBot: faixa de altura");
    ModelConfig cfg;
    // O OrcaSlicer espera layer_height em toda faixa
    const auto& print_cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    cfg.set_key_value("layer_height", new ConfigOptionFloat(print_cfg.opt_float("layer_height")));
    json result = apply_settings(cfg, p.value("settings", json::object()));
    obj->layer_config_ranges[{ z_min, z_max }].assign_config(std::move(cfg));
    refresh_after_model_change({ idx });
    return { {"index", idx}, {"range", {z_min, z_max}}, {"settings", result} };
}

json JusPrinUltra::AddModifierVolume(const json& p)
{
    const size_t idx = object_index(p);
    ModelObject* obj = model().objects[idx];
    const std::string kind = p.value("kind", std::string("modifier"));
    const std::string shape = p.value("shape", std::string("box"));
    const Vec3d size = xyz(p.value("size", json::object()), Vec3d(10, 10, 10));
    const Vec3d center = xyz(p.value("center", json::object()), obj->instance_bounding_box(0).center());

    ModelVolumeType type = ModelVolumeType::PARAMETER_MODIFIER;
    std::string name = "JusBot modificador";
    if (kind == "support_blocker") { type = ModelVolumeType::SUPPORT_BLOCKER; name = "JusBot bloqueio de suporte"; }
    else if (kind == "support_enforcer") { type = ModelVolumeType::SUPPORT_ENFORCER; name = "JusBot reforço de suporte"; }

    TriangleMesh mesh = shape == "cylinder"
        ? TriangleMesh(its_make_cylinder(std::max(size.x(), size.y()) / 2., size.z()))
        : TriangleMesh(its_make_cube(size.x(), size.y(), size.z()));
    mesh.translate(-mesh.bounding_box().center().cast<float>());

    plater()->take_snapshot("JusBot: adicionar " + name);
    ModelVolume* vol = obj->add_volume(std::move(mesh), type, false);
    vol->name = name;
    // Posiciona em coordenadas do mundo, desfazendo a transformação da instância
    const Transform3d inst_inv = obj->instances[0]->get_transformation().get_matrix().inverse();
    vol->set_transformation(Geometry::Transformation(inst_inv * Geometry::translation_transform(center)));
    json result = json::object();
    if (type == ModelVolumeType::PARAMETER_MODIFIER && p.contains("settings"))
        result = apply_settings(vol->config, p["settings"]);
    refresh_after_model_change({ idx });
    return { {"index", idx}, {"added", name}, {"settings", result} };
}

json JusPrinUltra::AddLayerGcode(const json& p)
{
    PartPlateList& list = plater()->get_partplate_list();
    const int plate = p.value("plate_index", list.get_curr_plate_index());
    const double z = p.value("z_mm", 0.0);
    if (z <= 0)
        fail("z_mm precisa ser maior que zero.");
    const std::string type = p.value("type", std::string("pause"));

    CustomGCode::Item item;
    item.print_z = z;
    item.extruder = 1;
    if (type == "color_change") {
        item.type = CustomGCode::ColorChange;
        item.color = "#FF8000";
    } else if (type == "custom") {
        item.type = CustomGCode::Custom;
        item.extra = p.value("gcode", std::string());
        if (item.extra.empty())
            fail("Para type=custom informe o gcode.");
    } else {
        item.type = CustomGCode::PausePrint;
        item.extra = "Pausa do JusBot";
    }

    plater()->take_snapshot("JusBot: ação na camada");
    CustomGCode::Info& info = model().plates_custom_gcodes[plate];
    if (info.mode == CustomGCode::Undef)
        info.mode = CustomGCode::SingleExtruder;
    info.gcodes.push_back(item);
    std::sort(info.gcodes.begin(), info.gcodes.end());
    plater()->schedule_background_process();
    return { {"plate_index", plate}, {"z_mm", z}, {"type", type}, {"total_actions", info.gcodes.size()},
             {"note", "Vale na primeira camada com altura >= z_mm. Fatie de novo para gerar o G-code."} };
}

json JusPrinUltra::AdaptiveLayerHeight(const json& p)
{
    const size_t idx = object_index(p);
    const float quality = std::clamp(p.value("quality", 0.5f), 0.f, 1.f);
    ModelObject* obj = model().objects[idx];

    plater()->take_snapshot("JusBot: camada adaptativa");
    const DynamicPrintConfig full = wxGetApp().preset_bundle->full_config();
    const SlicingParameters sp = PrintObject::slicing_parameters(
        full, *obj, float(obj->max_z()), plater()->fff_print().shrinkage_compensation());
    obj->layer_height_profile.set(layer_height_profile_adaptive(sp, *obj, quality));
    refresh_after_model_change({ idx });
    return { {"index", idx}, {"quality", quality} };
}

json JusPrinUltra::PlateOps(const json& p)
{
    PartPlateList& list = plater()->get_partplate_list();
    const std::string op = p.value("op", std::string());

    if (op == "add_plate") {
        plater()->take_snapshot("JusBot: nova mesa");
        const int created = list.create_plate();
        wxGetApp().obj_list()->reload_all_plates(false);
        return { {"created_plate_index", created}, {"plate_count", list.get_plate_count()} };
    }

    if (op == "move_object") {
        const size_t idx = object_index(p);
        const int target = p.value("plate_index", -1);
        PartPlate* plate = list.get_plate(target);
        if (!plate)
            fail("plate_index inválido.");
        plater()->take_snapshot("JusBot: mover para outra mesa");
        const BoundingBoxf3 box = plate->get_plate_box();
        ModelInstance* inst = model().objects[idx]->instances[0];
        inst->set_offset(Vec3d(box.center().x(), box.center().y(), inst->get_offset().z()));
        model().objects[idx]->invalidate_bounding_box();
        refresh_after_model_change({ idx }, true);
        return { {"index", idx}, {"plate_index", target} };
    }

    if (op == "set_bed_type") {
        static const std::map<std::string, BedType> names = {
            {"cool plate", btPC}, {"engineering plate", btEP}, {"high temp plate", btPEI},
            {"smooth pei plate", btPEI}, {"textured pei plate", btPTE}, {"textured cool plate", btPCT},
            {"supertack plate", btSuperTack},
        };
        const auto it = names.find(lower(p.value("bed_type", std::string())));
        if (it == names.end())
            fail("bed_type desconhecido. Use: Cool Plate, Engineering Plate, High Temp Plate, "
                 "Textured PEI Plate, Textured Cool Plate ou Supertack Plate.");
        const int target = p.value("plate_index", list.get_curr_plate_index());
        PartPlate* plate = list.get_plate(target);
        if (!plate)
            fail("plate_index inválido.");
        plate->set_bed_type(it->second);
        plater()->schedule_background_process();
        return { {"plate_index", target}, {"bed_type", p["bed_type"]} };
    }

    fail("op desconhecida: " + op);
}

json JusPrinUltra::ImportFiles(const json& p)
{
    std::vector<std::string> paths = p.value("paths", std::vector<std::string>{});
    if (paths.empty())
        fail("paths vazio.");
    for (const std::string& path : paths)
        if (!boost::filesystem::exists(boost::filesystem::path(path)))
            fail("Arquivo não encontrado: " + path);
    const std::vector<size_t> loaded = plater()->load_files(paths, LoadStrategy::LoadModel);
    return { {"loaded_object_indices", loaded}, {"objects_now", model().objects.size()} };
}

// ---------- Perfis ----------

json JusPrinUltra::ManagePreset(const json& p)
{
    const std::string op = p.value("op", std::string());
    const Preset::Type type = preset_type(p.value("type", std::string()));
    const std::string name = p.value("name", std::string());
    Tab* tab = wxGetApp().get_tab(type);
    if (!tab)
        fail("Aba do perfil não encontrada.");

    if (op == "save_as") {
        if (name.empty())
            fail("Informe o nome do perfil.");
        tab->save_preset(name);
        return { {"saved", name} };
    }
    if (op == "delete") {
        if (tab->get_presets()->get_edited_preset().name != name)
            tab->select_preset(name, false, std::string(), false);
        if (tab->get_presets()->get_edited_preset().name != name)
            fail("Perfil não encontrado: " + name);
        tab->delete_preset();   // o JusPrin pede confirmação ao usuário
        return { {"delete_requested", name} };
    }
    fail("op desconhecida: " + op + " (use save_as ou delete).");
}

json JusPrinUltra::ApplyPrinterConfig(const json& p)
{
    Tab* tab = wxGetApp().get_tab(Preset::TYPE_PRINTER);
    if (!tab || !tab->get_config())
        fail("Aba da impressora não encontrada.");
    json result = apply_settings(*tab->get_config(), p.value("settings", json::object()));
    tab->reload_config();
    tab->update();
    tab->update_dirty();
    return { {"settings", result} };
}

// ---------- Calibração ----------

json JusPrinUltra::RunCalibration(const json& p)
{
    const std::string test = p.value("test", std::string());
    Calib_Params params;
    params.start = p.value("start", 0.0);
    params.end = p.value("end", 0.0);
    params.step = p.value("step", 0.0);
    params.print_numbers = true;

    auto need_range = [&](double s, double e, double st) {
        if (!p.contains("start")) params.start = s;
        if (!p.contains("end")) params.end = e;
        if (!p.contains("step")) params.step = st;
    };

    if (test == "temperature") {
        // Faixa padrão do OrcaSlicer para PLA; o Claude deve informar a do filamento
        need_range(230, 190, 5);
        params.mode = CalibMode::Calib_Temp_Tower;
        plater()->calib_temp(params);
    } else if (test == "flow_coarse") {
        plater()->calib_flowrate(false, 1);
    } else if (test == "flow_fine") {
        plater()->calib_flowrate(false, 2);
    } else if (test == "pressure_advance") {
        need_range(0.0, 0.1, 0.002);
        params.mode = CalibMode::Calib_PA_Line;
        plater()->calib_pa(params);
    } else if (test == "retraction") {
        need_range(0.0, 2.0, 0.1);
        params.mode = CalibMode::Calib_Retraction_tower;
        plater()->calib_retraction(params);
    } else if (test == "max_volumetric_speed") {
        need_range(5, 20, 0.5);
        params.mode = CalibMode::Calib_Vol_speed_Tower;
        plater()->calib_max_vol_speed(params);
    } else if (test == "vfa") {
        need_range(40, 200, 10);
        params.mode = CalibMode::Calib_VFA_Tower;
        plater()->calib_VFA(params);
    } else if (test == "input_shaping_freq") {
        // Mesmos padrões do diálogo do OrcaSlicer: 15–110 Hz nos dois eixos, amortecimento 0,15
        params.freqStartX = params.freqStartY = p.value("freq_start", 15.0);
        params.freqEndX = params.freqEndY = p.value("freq_end", 110.0);
        params.start = p.value("damping", 0.15);
        params.test_model = 0;   // torre de ringing
        params.mode = CalibMode::Calib_Input_shaping_freq;
        plater()->calib_input_shaping_freq(params);
    } else if (test == "input_shaping_damp") {
        if (!p.contains("freq_x") || !p.contains("freq_y"))
            fail("Informe freq_x e freq_y (resultado do teste de frequência).");
        params.freqStartX = p.value("freq_x", 0.0);
        params.freqStartY = p.value("freq_y", 0.0);
        need_range(0.0, 0.5, 0.05);
        params.test_model = 0;
        params.mode = CalibMode::Calib_Input_shaping_damp;
        plater()->calib_input_shaping_damp(params);
    } else if (test == "junction_deviation") {
        need_range(0.0, 0.25, 0.01);
        params.test_model = 0;
        params.mode = CalibMode::Calib_Junction_Deviation;
        plater()->calib_junction_deviation(params);
    } else {
        fail("Teste desconhecido: " + test);
    }
    return { {"test", test}, {"start", params.start}, {"end", params.end}, {"step", params.step},
             {"next", "Fatie, imprima e conte ao JusBot o melhor resultado."} };
}

// ---------- Arquivos e impressora ----------

json JusPrinUltra::SaveProject(const json& p)
{
    const std::string format = p.value("format", std::string("3mf"));
    boost::filesystem::path path = p.value("path", std::string());
    if (path.empty()) {
        const boost::filesystem::path dir = boost::filesystem::path(wxStandardPaths::Get().GetDocumentsDir().ToUTF8().data()) / "JusPrin";
        boost::filesystem::create_directories(dir);
        path = dir / ("JusBot-" + timestamp() + "." + format);
    }

    if (format == "stl") {
        if (!store_stl(path.string().c_str(), &model(), true))
            fail("Falha ao salvar o STL.");
    } else if (format == "3mf") {
        const int ret = plater()->export_3mf(path);
        if (ret < 0)
            fail("Falha ao salvar o projeto (código " + std::to_string(ret) + ").");
    } else {
        fail("format precisa ser 3mf ou stl.");
    }
    return { {"saved", path.string()} };
}

json JusPrinUltra::SendToPrinter(const json&)
{
    if (!wxGetApp().preset_bundle->physical_printers.get_selected_printer_config())
        fail("Nenhuma impressora de rede configurada. No JusPrin, clique no ícone de Wi-Fi ao lado da "
             "impressora e cadastre o IP (tipo Creality Print ou Octo/Klipper).");
    PartPlate* plate = plater()->get_partplate_list().get_curr_plate();
    if (!plate || !plate->is_slice_result_ready_for_print())
        fail("Fatie a mesa antes de enviar.");
    plater()->send_gcode_legacy(-1);   // abre a janela de envio para o usuário confirmar
    return { {"status", "Janela de envio aberta para o usuário confirmar."} };
}

// ---------- Verificação e reparo de modelo ----------

json JusPrinUltra::CheckModel(const json& p)
{
    json objects = json::array();
    const auto& objs = model().objects;
    for (size_t i = 0; i < objs.size(); ++i) {
        if (p.contains("index") && p["index"].get<int>() != int(i))
            continue;
        const ModelObject* obj = objs[i];
        const TriangleMeshStats st = obj->get_object_stl_stats();
        const RepairedMeshErrors& fixed = st.repaired_errors;
        objects.push_back({
            {"index", i}, {"name", obj->name},
            {"facets", st.number_of_facets}, {"volume_mm3", st.volume},
            {"separate_shells", st.number_of_parts},
            {"open_edges", st.open_edges},
            {"watertight", st.open_edges == 0},
            {"auto_fixed_on_import", {
                {"edges_fixed", fixed.edges_fixed}, {"degenerate_facets", fixed.degenerate_facets},
                {"facets_removed", fixed.facets_removed}, {"facets_reversed", fixed.facets_reversed}}},
            {"needs_repair", st.open_edges > 0},
        });
    }
    return { {"objects", objects},
             {"can_repair", plater()->can_fix_through_netfabb()},
             {"note", "open_edges > 0 indica furos/bordas abertas; várias cascas separadas podem ser normais."} };
}

json JusPrinUltra::RepairModel(const json& p)
{
    const size_t idx = object_index(p);
    ModelObject* obj = model().objects[idx];
    const int before = obj->get_object_stl_stats().open_edges;

    plater()->take_snapshot("JusBot: reparar modelo");
    ProgressDialog progress_dlg(_L("Repairing model object"), "", 100, wxGetApp().mainframe,
                                wxPD_AUTO_HIDE | wxPD_APP_MODAL | wxPD_CAN_ABORT);
    std::string fix_result;
    const bool ok = fix_model_by_win10_sdk_gui(*obj, -1, progress_dlg, _L("Repairing model object"), fix_result);
    wxGetApp().obj_list()->update_item_error_icon(int(idx), -1);
    refresh_after_model_change({ idx });
    const int after = model().objects[idx]->get_object_stl_stats().open_edges;
    if (!ok)
        fail("O reparo do Windows falhou: " + fix_result);
    return { {"index", idx}, {"open_edges_before", before}, {"open_edges_after", after}, {"details", fix_result} };
}

// ---------- Imagem da pré-visualização fatiada ----------

namespace {
std::string pixels_to_jpeg_base64(const std::vector<unsigned char>& px, int w, int h, int max_w)
{
    wxImage image(w, h);
    for (int r = 0; r < h; ++r) {
        const unsigned char* row = px.data() + size_t(h - 1 - r) * w * 4;   // OpenGL lê de baixo para cima
        for (int c = 0; c < w; ++c)
            image.SetRGB(c, r, row[c * 4], row[c * 4 + 1], row[c * 4 + 2]);
    }
    if (w > max_w)
        image.Rescale(max_w, h * max_w / w, wxIMAGE_QUALITY_HIGH);
    image.SetOption(wxIMAGE_OPTION_QUALITY, 85);
    wxMemoryOutputStream stream;
    image.SaveFile(stream, wxBITMAP_TYPE_JPEG);
    wxStreamBuffer* buf = stream.GetOutputStreamBuffer();
    const size_t size = buf->GetBufferSize();
    std::string b64(boost::beast::detail::base64::encoded_size(size), '\0');
    b64.resize(boost::beast::detail::base64::encode(b64.data(), buf->GetBufferStart(), size));
    return "data:image/jpeg;base64," + b64;
}
} // namespace

json JusPrinUltra::RenderPreview(const json& p)
{
    PartPlate* plate = plater()->get_partplate_list().get_curr_plate();
    if (!plate || !plate->is_slice_result_valid())
        fail("A mesa ainda não foi fatiada. Use slice antes.");

    GLCanvas3D* canvas = plater()->get_preview_canvas3D();
    if (!canvas || !canvas->get_wxglcanvas()->IsShownOnScreen()) {
        // A pré-visualização só desenha quando está visível: abre a aba e pede para tentar de novo
        plater()->select_view_3D("Preview", false);
        wxGetApp().mainframe->select_tab(MainFrame::tpPreview);
        fail("Abri a aba de pré-visualização agora; chame view_sliced_preview de novo.");
    }

    std::vector<std::string> views = p.value("views", std::vector<std::string>{"iso", "top", "front"});
    json images = json::array();
    for (const std::string& view : views) {
        canvas->select_view(view);
        canvas->zoom_to_gcode();
        canvas->render();
        const Size size = canvas->get_canvas_size();
        const int w = size.get_width(), h = size.get_height();
        if (w <= 0 || h <= 0)
            fail("Não consegui ler a imagem da pré-visualização.");
        std::vector<unsigned char> px(size_t(w) * h * 4);
        glsafe(::glReadBuffer(GL_FRONT));
        glsafe(::glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data()));
        images.push_back({ {"view", view}, {"base64", pixels_to_jpeg_base64(px, w, h, 1280)} });
    }
    return images;
}

}} // namespace Slic3r::GUI
