#include "CopilotTask.h"

#include <algorithm>
#include <cstdint>

#include "Arknights-Tile-Pos/TileCalc2.hpp"

#include "Common/AsstBattleDef.h"
#include "Config/GeneralConfig.h"
#include "Config/Miscellaneous/BattleDataConfig.h"
#include "Config/Miscellaneous/CopilotConfig.h"
#include "Config/Miscellaneous/OperBoxDataConfig.h"
#include "Config/TaskData.h"
#include "Task/Fight/MedicineCounterTaskPlugin.h"
#include "Task/Miscellaneous/BattleFormationTask.h"
#include "Task/Miscellaneous/BattleProcessTask.h"
#include "Task/Miscellaneous/MultiCopilotTaskPlugin.h"
#include "Task/ProcessTask.h"
#include "Utils/Logger.hpp"
#include "Utils/Platform.hpp"

asst::CopilotTask::CopilotTask(const AsstCallback& callback, Assistant* inst) :
    InterfaceTask(callback, inst, TaskType),
    m_multi_copilot_plugin_ptr(std::make_shared<MultiCopilotTaskPlugin>(callback, inst, TaskType)),
    m_formation_task_ptr(std::make_shared<BattleFormationTask>(callback, inst, TaskType)),
    m_battle_task_ptr(std::make_shared<BattleProcessTask>(callback, inst, TaskType)),
    m_stop_task_ptr(std::make_shared<ProcessTask>(callback, inst, TaskType))
{
    LogTraceFunction;

    m_multi_copilot_plugin_ptr->set_retry_times(0);
    m_multi_copilot_plugin_ptr->set_formation_task_ptr(m_formation_task_ptr);
    m_multi_copilot_plugin_ptr->set_battle_task_ptr(m_battle_task_ptr);
    m_subtasks.emplace_back(m_multi_copilot_plugin_ptr);

    auto start_1_tp = std::make_shared<ProcessTask>(callback, inst, TaskType);
    start_1_tp->set_tasks({ "BattleStartPre" }).set_retry_times(3).set_ignore_error(true);
    m_subtasks.emplace_back(start_1_tp);

    m_medicine_task_ptr = std::make_shared<ProcessTask>(callback, inst, TaskType);
    m_medicine_task_ptr->set_tasks({ "BattleStartPre@UseMedicine", "BattleStartPre@BattleQuickFormation" })
        .set_ignore_error(true);
    m_medicine_task_ptr->register_plugin<MedicineCounterTaskPlugin>()->set_count(999'999);
    m_subtasks.emplace_back(m_medicine_task_ptr);

    m_subtasks.emplace_back(m_formation_task_ptr)->set_retry_times(0);

    auto start_2_tp = std::make_shared<ProcessTask>(callback, inst, TaskType);
    start_2_tp->set_tasks({ "BattleStartAll" }).set_retry_times(3).set_ignore_error(false);
    m_subtasks.emplace_back(start_2_tp);

    // 跳过“以下干员出战后将被禁用，是否继续？”对话框
    auto start_3_tp = std::make_shared<ProcessTask>(callback, inst, TaskType);
    start_3_tp->set_tasks({ "SkipForbiddenOperConfirm", "Stop" }).set_ignore_error(false);
    m_subtasks.emplace_back(start_3_tp);

    m_subtasks.emplace_back(m_battle_task_ptr)->set_retry_times(0);

    m_stop_task_ptr->set_enable(false);
    m_subtasks.emplace_back(m_stop_task_ptr);
}

bool asst::CopilotTask::set_params(const json::value& params)
{
    LogTraceFunction;

    using SupportUnitUsage = BattleFormationTask::SupportUnitUsage;
    using OperUsageGroups = battle::copilot::OperUsageGroups;

    if (m_params_locked) {
        Log.error(__FUNCTION__, "CopilotTask set_params failed, already set params");
        return false;
    }

    bool use_sanity_potion = params.get("use_sanity_potion", false);                 // 是否使用理智药
    bool with_formation = params.get("formation", false);                            // 是否使用自动编队
    int formation_index = params.get("formation_index", 0);                          // 选择第几个编队，0为不选择
    bool add_trust = params.get("add_trust", false);                                 // 是否自动补信赖
    bool ignore_requirements = params.get("ignore_requirements", false);             // 跳过未满足的干员属性要求
    bool add_user_additional = params.contains("user_additional");                   // 是否自动补用户自定义干员
    auto support_unit_usage = static_cast<SupportUnitUsage>(
        params.get("support_unit_usage", static_cast<int>(SupportUnitUsage::None))); // 助战干员使用模式
    std::string support_unit_name = params.get("support_unit_name", std::string());
    std::string operbox_data_path = params.get("operbox_data_path", std::string());  // 干员辅助编队数据路径, 为空则禁用
    bool use_operbox = !operbox_data_path.empty();

    if (use_operbox) {
        // 预检失败回调按编队缺干员协议从编队任务对象发出，GUI 侧直接复用 OperatorMissing 分支处理
        OperBoxData.set_task(m_formation_task_ptr.get());
        OperBoxData.set_ignore_requirements(ignore_requirements);
        if (!OperBoxData.load(utils::path(operbox_data_path))) {
            LogError << __FUNCTION__ << "| OperBox data is empty or invalid, cannot perform precheck";
            json::value info = basic_info_with_what("OperboxDataParseFailed");
            callback(AsstMsg::SubTaskError, info);
            return false;
        }
    }

    auto filename_opt = params.find<std::string>("filename");
    auto multi_tasks_opt = params.find<json::array>("copilot_list"); // 多任务列表
    if (!filename_opt && !multi_tasks_opt) {
        LogError << __FUNCTION__ << "CopilotTask set_params failed, stage_name or filename not found";
        return false;
    }

    const auto max_failure_retries = params.find<int>("max_failure_retries");
    if (params.contains("max_failure_retries") && (!max_failure_retries || *max_failure_retries < 0)) {
        LogError << __FUNCTION__ << "max_failure_retries must be a non-negative integer";
        return false;
    }
    const bool use_copilot_list = !filename_opt && multi_tasks_opt.has_value();
    const bool continue_on_two_stars = use_copilot_list && params.get("continue_on_two_stars", false);
    const bool retry_on_failure = params.get("retry_on_failure", max_failure_retries.has_value());
    const bool retry_on_leak = params.get("retry_on_leak", false);
    const bool manage_battle_result = retry_on_failure || retry_on_leak || continue_on_two_stars;

    if (filename_opt) {
        m_multi_copilot_plugin_ptr->set_enable(false);
        m_battle_task_ptr->set_wait_until_end(manage_battle_result);
        auto copilot_opt = parse_copilot_filename(*filename_opt);
        if (!copilot_opt) {
            return false;
        }
        if (use_operbox) {
            auto assignment_opt =
                OperBoxData.precheck(Copilot.get_data().groups, support_unit_usage != SupportUnitUsage::None);
            if (!assignment_opt) {
                return false;
            }
            m_formation_task_ptr->set_assigned_groups(std::move(*assignment_opt));
        }
        else {
            m_formation_task_ptr->set_assigned_groups(std::nullopt);
        }
        m_stage_name = Copilot.get_stage_name();
        if (!m_battle_task_ptr->set_stage_name(m_stage_name)) {
            Log.error("Not support stage");
            json::value info = basic_info_with_what("CopilotStageNotSupported");
            info["details"]["stage_name"] = m_stage_name;
            callback(AsstMsg::SubTaskError, info);
            return false;
        }
    }
    else if (multi_tasks_opt) {
        m_multi_copilot_plugin_ptr->set_enable(true); // 启用多任务插件, 自动覆盖Copilot中的配置
        m_battle_task_ptr->set_wait_until_end(true);
        auto configs = static_cast<std::vector<MultiCopilotConfig>>(*multi_tasks_opt);
        if (configs.empty()) {
            LogError << __FUNCTION__ << "copilot_list must contain at least one stage";
            return false;
        }
        std::vector<MultiCopilotTaskPlugin::MultiCopilotConfig> configs_cvt;
        std::unordered_map<std::string, std::shared_ptr<OperUsageGroups>> operbox_assignments;
        for (const auto& [id, filename, nav_name, is_raid] : configs) {
            MultiCopilotTaskPlugin::MultiCopilotConfig config_cvt;
            auto copilot_opt = parse_copilot_filename(filename);
            if (!copilot_opt) {
                return false;
            }
            const auto& stage_name = Copilot.get_stage_name();
            const auto& map_data = Tile.find(stage_name);
            if (!map_data || !json::open(map_data->second)) {
                return false;
            }
            if (!nav_name) {
                config_cvt.nav_name = map_data->first.code;
            }
            else {
                LogInfo << __FUNCTION__ << " | navigation name override: " << *nav_name;
                config_cvt.nav_name = *nav_name;
            }
            config_cvt.assigned_groups = nullptr;
            if (use_operbox) {
                if (operbox_assignments.find(filename) != operbox_assignments.end()) {
                    config_cvt.assigned_groups = operbox_assignments[filename];
                }
                else {
                    json::value info = basic_info_with_what("CopilotPrecheckFile");
                    info["details"]["nav_name"] = config_cvt.nav_name;
                    info["details"]["filename"] = filename;
                    callback(AsstMsg::SubTaskExtraInfo, info);
                    auto assignment_opt =
                        OperBoxData.precheck(Copilot.get_data().groups, support_unit_usage != SupportUnitUsage::None);
                    if (!assignment_opt) {
                        return false;
                    }
                    config_cvt.assigned_groups = std::make_shared<OperUsageGroups>(std::move(*assignment_opt));
                    operbox_assignments[filename] = config_cvt.assigned_groups;
                }
            }
            config_cvt.copilot_file = *copilot_opt;
            config_cvt.is_raid = is_raid;
            config_cvt.id = id; // ID 从0开始
            configs_cvt.emplace_back(std::move(config_cvt));
        }

        const size_t count = configs_cvt.size();
        m_multi_copilot_plugin_ptr->set_multi_copilot_config(std::move(configs_cvt));
        m_multi_copilot_plugin_ptr->set_stage_index(0);

        for (const auto& obj : *multi_tasks_opt) {
            if (obj.contains("is_paradox")) {
                Log.error("================  !DEPRECATED!  ================");
                LogError << "`is_paradox` has been deprecated since v6.1.2;";
                LogError << "Please use 'ParadoxCopilotTask' for paradox copilot;";
                Log.error("================  !DEPRECATED!  ================");
                return false;
            }
        }

        if (!manage_battle_result) {
            const auto stage_tasks = m_subtasks;
            m_subtasks.reserve(stage_tasks.size() * count);
            for (size_t i = 1; i < count; ++i) {
                m_subtasks.insert(m_subtasks.end(), stage_tasks.begin(), stage_tasks.end());
            }
        }
    }

    m_medicine_task_ptr->set_enable(use_sanity_potion);

    m_formation_task_ptr->set_enable(with_formation);
    m_formation_task_ptr->set_select_formation(formation_index);
    m_formation_task_ptr->set_add_trust(add_trust);
    m_formation_task_ptr->set_ignore_requirements(ignore_requirements);
    m_formation_task_ptr->set_support_unit_usage(support_unit_usage);
    m_formation_task_ptr->set_specific_support_unit(support_unit_name);

    if (auto opt = params.find<json::array>("user_additional"); with_formation && add_user_additional && opt) {
        std::vector<std::pair<std::string, int>> user_additional;
        for (const auto& op : *opt) {
            std::string name = op.get("name", std::string());
            if (name.empty()) {
                continue;
            }
            if (BattleData.is_name_invalid(battle::Role::Unknown, name)) {
                LogError << __FUNCTION__ << "| User additional oper" << name << "is invalid";
                json::value info = basic_info_with_what("UserAdditionalOperInvalid");
                info["details"]["name"] = name;
                callback(AsstMsg::SubTaskError, info);
                return false;
            }
            user_additional.emplace_back(std::pair<std::string, int> { std::move(name), op.get("skill", 0) });
        }
        m_formation_task_ptr->set_user_additional(std::move(user_additional));
    }

    m_battle_task_ptr->set_formation_task_ptr(m_formation_task_ptr->get_opers_in_formation());

    const int loop_times = params.get("loop_times", 1);
    m_use_copilot_list = use_copilot_list;
    m_continue_on_two_stars = continue_on_two_stars;
    m_retry_on_failure = retry_on_failure;
    m_max_retries = max_failure_retries.value_or(1);
    m_battle_task_ptr->set_retry_on_leak(retry_on_leak);
    m_managed_stage_count = 0;
    m_stop_task_ptr->set_enable(false);
    if (manage_battle_result) {
        m_managed_stage_count =
            use_copilot_list ? multi_tasks_opt->size() : static_cast<size_t>(std::max(1, loop_times));
    }
    else if (use_copilot_list) {
        m_stop_task_ptr->set_tasks({ "Copilot@WaitUntilEndOfAction" });
        m_stop_task_ptr->set_enable(true);
    }
    else if (loop_times > 1) {
        m_stop_task_ptr->set_tasks({ "ClickCornerUntilStartButton" });
        m_stop_task_ptr->set_enable(true);

        const auto stage_tasks = m_subtasks;
        m_subtasks.reserve(stage_tasks.size() * static_cast<size_t>(loop_times));
        for (int i = 1; i < loop_times; ++i) {
            m_subtasks.insert(m_subtasks.end(), stage_tasks.begin(), stage_tasks.end());
        }
    }
    m_params_locked = use_copilot_list || loop_times > 1;
    return true;
}

bool asst::CopilotTask::run()
{
    if (!m_enable || m_managed_stage_count == 0) {
        return InterfaceTask::run();
    }

    std::vector<bool> skipped_stages(m_managed_stage_count);
    for (size_t i = 0; i < m_managed_stage_count; ++i) {
        if (!skipped_stages[i] && !run_managed_stage(i, skipped_stages)) {
            return false;
        }
    }
    return true;
}

bool asst::CopilotTask::run_managed_stage(size_t index, std::vector<bool>& skipped_stages)
{
    constexpr std::string_view Failed = "Copilot@BattleResultFailed";
    constexpr std::string_view FailureConfirmed = "Copilot@BattleFailureConfirmed";
    constexpr std::string_view ThreeStars = "Copilot@BattleResultThreeStars";
    constexpr std::string_view Adverse = "Copilot@BattleResultAdverse";
    constexpr std::string_view TwoStars = "Copilot@BattleResultTwoStars";
    constexpr std::string_view Sandbox = "Copilot@BattleResultSandbox";

    m_multi_copilot_plugin_ptr->set_enable(m_use_copilot_list);
    const auto* config = m_use_copilot_list ? &m_multi_copilot_plugin_ptr->get_config(index) : nullptr;
    for (std::uint64_t retries = 0; !need_exit(); ++retries) {
        m_multi_copilot_plugin_ptr->set_stage_index(index);
        if (!InterfaceTask::run() || need_exit()) {
            return false;
        }

        const bool enemy_leaked = m_battle_task_ptr->enemy_leaked();
        std::string result;
        if (!enemy_leaked) {
            ProcessTask settlement(*this, { "Copilot@WaitForBattleResult" });
            if (!settlement.run() || need_exit()) {
                return false;
            }
            result = settlement.get_last_task_name();
        }
        const bool failed = enemy_leaked || result == Failed || result == FailureConfirmed;
        const bool two_stars = result == TwoStars;
        if (!failed && !two_stars && result != ThreeStars && result != Adverse && result != Sandbox) {
            LogError << __FUNCTION__ << "Could not recognize the battle result" << result;
            return false;
        }
        if ((failed && !enemy_leaked && !m_retry_on_failure) || (two_stars && config && !m_continue_on_two_stars)) {
            return false;
        }
        const bool retry = failed && (m_max_retries == 0 || retries < static_cast<std::uint64_t>(m_max_retries));

        if (!enemy_leaked && (config || failed || index + 1 < m_managed_stage_count)) {
            ProcessTask return_to_stage(*this, { "Copilot@ClickCornerUntilStartButton" });
            return_to_stage.set_times_limit("Copilot@ClickCornerUntilStartButton", 15);
            if (!return_to_stage.run() || need_exit()) {
                return false;
            }
            const auto& page = return_to_stage.get_last_task_name();
            if (page != "Copilot@BattleStartPreFlag" && page != "Copilot@BattleStartPreFlag-Return") {
                LogError << __FUNCTION__ << "Could not leave the battle results";
                return false;
            }
        }

        if (result != Sandbox) {
            json::array skipped_raid_ids;
            if (two_stars && config && !config->is_raid) {
                for (size_t i = index + 1; i < m_managed_stage_count; ++i) {
                    const auto& queued = m_multi_copilot_plugin_ptr->get_config(i);
                    if (!skipped_stages[i] && queued.is_raid && queued.nav_name == config->nav_name) {
                        skipped_stages[i] = true;
                        skipped_raid_ids.emplace_back(queued.id);
                    }
                }
            }

            auto info = basic_info_with_what("CopilotBattleResult");
            auto& details = info["details"];
            details["id"] = config ? config->id : -1;
            details["stage_name"] = config ? config->nav_name : m_stage_name;
            details["stars"] = failed ? 0 : two_stars ? 2 : 3;
            details["enemy_leaked"] = enemy_leaked;
            details["retry_number"] = retry ? retries + 1 : 0;
            details["skipped_raid_ids"] = std::move(skipped_raid_ids);
            callback(AsstMsg::SubTaskExtraInfo, info);
        }

        if (!retry) {
            return !failed || config != nullptr;
        }
        if (!sleep(Config.get_options().task_delay)) {
            return false;
        }
    }
    return false;
}

std::optional<std::filesystem::path> asst::CopilotTask::parse_copilot_filename(const std::string& name)
{
    auto path = utils::path(name);
    if (!Copilot.load(path)) {
        Log.error("CopilotConfig parse failed");
        // 发结构化错误让 GUI 报准确原因，避免落到通用启动失败兜底
        json::value info = basic_info_with_what("CopilotFileReadError");
        callback(AsstMsg::SubTaskError, info);
        return std::nullopt;
    }
    return path;
}
