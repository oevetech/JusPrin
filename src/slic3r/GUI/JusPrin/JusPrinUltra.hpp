#ifndef slic3r_JusPrinUltra_hpp_
#define slic3r_JusPrinUltra_hpp_

#include <functional>
#include <map>
#include <string>
#include "nlohmann/json.hpp"

namespace Slic3r { namespace GUI {

// Ações do "modo Ultra" do chat (JusBot + Claude).
// Cada ação recebe o payload JSON da página do chat e devolve JSON, ou lança
// std::runtime_error com uma mensagem que é repassada ao chat como erro.
// Os nomes (jp_*) são o contrato com servidor.py (ULTRA_TOOLS).
class JusPrinUltra
{
public:
    using Handler = std::function<nlohmann::json(const nlohmann::json& payload)>;

    static const std::map<std::string, Handler>& handlers();

    static nlohmann::json GetSliceStats(const nlohmann::json& payload);
    static nlohmann::json GetLayerStats(const nlohmann::json& payload);
    static nlohmann::json TransformObjects(const nlohmann::json& payload);
    static nlohmann::json SetInstances(const nlohmann::json& payload);
    static nlohmann::json DeleteObjects(const nlohmann::json& payload);
    static nlohmann::json CutObject(const nlohmann::json& payload);
    static nlohmann::json SetObjectConfig(const nlohmann::json& payload);
    static nlohmann::json AddHeightRange(const nlohmann::json& payload);
    static nlohmann::json AddModifierVolume(const nlohmann::json& payload);
    static nlohmann::json AddLayerGcode(const nlohmann::json& payload);
    static nlohmann::json AdaptiveLayerHeight(const nlohmann::json& payload);
    static nlohmann::json PlateOps(const nlohmann::json& payload);
    static nlohmann::json ImportFiles(const nlohmann::json& payload);
    static nlohmann::json ManagePreset(const nlohmann::json& payload);
    static nlohmann::json ApplyPrinterConfig(const nlohmann::json& payload);
    static nlohmann::json RunCalibration(const nlohmann::json& payload);
    static nlohmann::json SaveProject(const nlohmann::json& payload);
    static nlohmann::json SendToPrinter(const nlohmann::json& payload);
};

}} // namespace Slic3r::GUI

#endif /* slic3r_JusPrinUltra_hpp_ */
