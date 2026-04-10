/**
 * RDMA 错误码诊断工具
 *
 * 功能：
 *   - 查询所有 IBV_WC_* 错误码的含义、常见原因、修复建议
 *   - 查询所有异步事件码 (IBV_EVENT_*) 的含义
 *   - 支持两种使用方式:
 *     a) ./error_diagnosis          → 打印所有错误码
 *     b) ./error_diagnosis <code>   → 查询特定错误码
 *
 * 编译:
 *   gcc -o error_diagnosis error_diagnosis.c -libverbs \
 *       -L../../common -lrdma_utils -I../../common -O2
 *
 * 运行:
 *   ./error_diagnosis            # 列出所有错误码
 *   ./error_diagnosis 5          # 查询 status=5 的详情
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <infiniband/verbs.h>
#include "rdma_utils.h"

/* ========== 颜色定义 (终端) ========== */
#define C_RED     "\033[0;31m"
#define C_GREEN   "\033[0;32m"
#define C_YELLOW  "\033[1;33m"
#define C_BLUE    "\033[0;34m"
#define C_CYAN    "\033[0;36m"
#define C_BOLD    "\033[1m"
#define C_NC      "\033[0m"

/* ========== WC 错误码信息结构 ========== */
struct wc_error_info {
    int         status;         /* IBV_WC_* 枚举值 */
    const char *name;           /* 枚举名 */
    const char *meaning_cn;     /* 中文含义 */
    const char *cause1;         /* 常见原因 1 */
    const char *cause2;         /* 常见原因 2 */
    const char *cause3;         /* 常见原因 3 */
    const char *fix;            /* 修复建议 */
};

/* ========== 完整的 WC 错误码表 ========== */
static const struct wc_error_info wc_errors[] = {
    {
        .status     = IBV_WC_SUCCESS,                /* 0 */
        .name       = "IBV_WC_SUCCESS",
        .meaning_cn = "操作成功完成",
        .cause1     = "(无错误)",
        .cause2     = NULL,
        .cause3     = NULL,
        .fix        = "无需修复",
    },
    {
        .status     = IBV_WC_LOC_LEN_ERR,            /* 1 */
        .name       = "IBV_WC_LOC_LEN_ERR",
        .meaning_cn = "本地长度错误",
        .cause1     = "接收缓冲区太小，无法容纳收到的消息",
        .cause2     = "Send 的 SGE 总长度超过 QP 的 max_inline_data (inline 模式)",
        .cause3     = "RDMA Read 响应数据超过本地缓冲区大小",
        .fix        = "增大 recv buffer 大小; 检查 SGE 长度与缓冲区匹配",
    },
    {
        .status     = IBV_WC_LOC_QP_OP_ERR,          /* 2 */
        .name       = "IBV_WC_LOC_QP_OP_ERR",
        .meaning_cn = "本地 QP 操作错误",
        .cause1     = "QP 配置与请求的操作不兼容 (如 UD QP 发 RDMA Write)",
        .cause2     = "内部 QP 一致性错误",
        .cause3     = "超过 QP 的 max_rd_atomic / max_dest_rd_atomic 限制",
        .fix        = "检查 QP 类型是否支持该操作; 检查 QP 属性配置",
    },
    {
        .status     = IBV_WC_LOC_EEC_OP_ERR,         /* 3 */
        .name       = "IBV_WC_LOC_EEC_OP_ERR",
        .meaning_cn = "本地 EEC 操作错误 (RD 模式, 已废弃)",
        .cause1     = "EE 上下文操作失败 (Reliable Datagram 模式)",
        .cause2     = NULL,
        .cause3     = NULL,
        .fix        = "RD 模式已很少使用, 建议使用 RC 或 UD 模式",
    },
    {
        .status     = IBV_WC_LOC_PROT_ERR,           /* 4 */
        .name       = "IBV_WC_LOC_PROT_ERR",
        .meaning_cn = "本地保护域错误",
        .cause1     = "lkey 对应的 MR 与 QP 不在同一个 PD (保护域不匹配)",
        .cause2     = "lkey 无效、已被 deregister、或已过期",
        .cause3     = "SGE 中的地址超出 MR 注册范围",
        .fix        = "确认 QP 和 MR 属于同一个 PD; 检查 lkey 和地址范围",
    },
    {
        .status     = IBV_WC_WR_FLUSH_ERR,           /* 5 */
        .name       = "IBV_WC_WR_FLUSH_ERR",
        .meaning_cn = "WR 被刷出 (QP 已处于 ERROR 状态)",
        .cause1     = "QP 因之前的错误已进入 ERROR 状态",
        .cause2     = "该 WR 及所有后续 WR 都会被标记为 FLUSH_ERR",
        .cause3     = "这是继发错误，根本原因是之前的那个错误",
        .fix        = "找到第一个非 FLUSH_ERR 的错误并修复; 恢复 QP: ERROR→RESET→INIT→RTR→RTS",
    },
    {
        .status     = IBV_WC_MW_BIND_ERR,            /* 6 */
        .name       = "IBV_WC_MW_BIND_ERR",
        .meaning_cn = "Memory Window 绑定错误",
        .cause1     = "MW 绑定参数无效",
        .cause2     = "MW 的 PD 与 QP 不匹配",
        .cause3     = "底层 MR 权限不足",
        .fix        = "检查 MW 参数和权限配置",
    },
    {
        .status     = IBV_WC_BAD_RESP_ERR,           /* 7 */
        .name       = "IBV_WC_BAD_RESP_ERR",
        .meaning_cn = "错误的响应 (协议错误)",
        .cause1     = "收到了意外的 opcode 响应",
        .cause2     = "对端发送了不合法的响应包",
        .cause3     = "网络中的数据损坏",
        .fix        = "检查两端 QP 配置是否一致; 检查网络连通性",
    },
    {
        .status     = IBV_WC_LOC_ACCESS_ERR,         /* 8 */
        .name       = "IBV_WC_LOC_ACCESS_ERR",
        .meaning_cn = "本地访问权限错误",
        .cause1     = "MR 没有 LOCAL_WRITE 权限，但 recv 需要写入",
        .cause2     = "Atomic 操作的本地缓冲区没有写权限",
        .cause3     = NULL,
        .fix        = "注册 MR 时添加 IBV_ACCESS_LOCAL_WRITE",
    },
    {
        .status     = IBV_WC_REM_INV_REQ_ERR,       /* 9 */
        .name       = "IBV_WC_REM_INV_REQ_ERR",
        .meaning_cn = "远端无效请求错误",
        .cause1     = "请求的远端虚拟地址无效",
        .cause2     = "RDMA 操作的 length 为 0 或超出范围",
        .cause3     = "操作违反了远端 QP 的配置限制",
        .fix        = "检查远端地址和长度; 确认远端 QP 已正确配置",
    },
    {
        .status     = IBV_WC_REM_ACCESS_ERR,         /* 10 */
        .name       = "IBV_WC_REM_ACCESS_ERR",
        .meaning_cn = "远端访问权限错误",
        .cause1     = "rkey 错误或已失效",
        .cause2     = "目标地址超出远端 MR 注册范围",
        .cause3     = "远端 MR 没有 REMOTE_WRITE/REMOTE_READ 权限",
        .fix        = "确认 rkey 正确; 检查远端 MR 的 IBV_ACCESS_REMOTE_* 标志",
    },
    {
        .status     = IBV_WC_REM_OP_ERR,             /* 11 */
        .name       = "IBV_WC_REM_OP_ERR",
        .meaning_cn = "远端操作错误",
        .cause1     = "远端无法完成请求的操作",
        .cause2     = "远端 QP 出现内部错误",
        .cause3     = "远端资源不足",
        .fix        = "检查远端 QP 状态; 检查远端日志",
    },
    {
        .status     = IBV_WC_RETRY_EXC_ERR,          /* 12 */
        .name       = "IBV_WC_RETRY_EXC_ERR",
        .meaning_cn = "重试次数超限 (对端不可达)",
        .cause1     = "对端机器宕机或网络中断",
        .cause2     = "对端 QP 未正确配置 (未到 RTS 状态)",
        .cause3     = "防火墙阻断了 RDMA 流量 (RoCE 需要放行 UDP 4791)",
        .fix        = "确认对端在线; 检查网络连通性 (ping); 检查 QP 状态",
    },
    {
        .status     = IBV_WC_RNR_RETRY_EXC_ERR,      /* 13 */
        .name       = "IBV_WC_RNR_RETRY_EXC_ERR",
        .meaning_cn = "RNR (Receiver Not Ready) 重试超限",
        .cause1     = "对端没有 post recv 就收到了消息",
        .cause2     = "对端的 recv buffer 已经全部消耗完",
        .cause3     = "rnr_retry 设置为 0 (不重试)",
        .fix        = "确保对端先 post_recv; 设置 rnr_retry=7 (无限重试)",
    },
    {
        .status     = IBV_WC_LOC_RDD_VIOL_ERR,       /* 14 */
        .name       = "IBV_WC_LOC_RDD_VIOL_ERR",
        .meaning_cn = "本地 RDD 违规错误 (RD 模式, 已废弃)",
        .cause1     = "Reliable Datagram 域不匹配",
        .cause2     = NULL,
        .cause3     = NULL,
        .fix        = "RD 模式已废弃, 建议使用 RC 模式",
    },
    {
        .status     = IBV_WC_REM_INV_RD_REQ_ERR,     /* 15 */
        .name       = "IBV_WC_REM_INV_RD_REQ_ERR",
        .meaning_cn = "远端无效 RD 请求错误 (RD 模式, 已废弃)",
        .cause1     = "RD 请求无效",
        .cause2     = NULL,
        .cause3     = NULL,
        .fix        = "RD 模式已废弃",
    },
    {
        .status     = IBV_WC_REM_ABORT_ERR,           /* 16 */
        .name       = "IBV_WC_REM_ABORT_ERR",
        .meaning_cn = "远端中止错误",
        .cause1     = "远端主动中止了操作",
        .cause2     = "远端 QP 被销毁或重置",
        .cause3     = NULL,
        .fix        = "检查远端应用是否正常退出",
    },
    {
        .status     = IBV_WC_INV_EECN_ERR,           /* 17 */
        .name       = "IBV_WC_INV_EECN_ERR",
        .meaning_cn = "无效 EEC 编号错误 (已废弃)",
        .cause1     = "EE 上下文编号无效",
        .cause2     = NULL,
        .cause3     = NULL,
        .fix        = "已废弃的错误码",
    },
    {
        .status     = IBV_WC_INV_EEC_STATE_ERR,      /* 18 */
        .name       = "IBV_WC_INV_EEC_STATE_ERR",
        .meaning_cn = "无效 EEC 状态错误 (已废弃)",
        .cause1     = "EE 上下文处于错误状态",
        .cause2     = NULL,
        .cause3     = NULL,
        .fix        = "已废弃的错误码",
    },
    {
        .status     = IBV_WC_FATAL_ERR,              /* 19 */
        .name       = "IBV_WC_FATAL_ERR",
        .meaning_cn = "致命错误",
        .cause1     = "不可恢复的传输错误",
        .cause2     = "硬件故障",
        .cause3     = "驱动内部严重错误",
        .fix        = "检查 dmesg 日志; 可能需要重启驱动或系统",
    },
    {
        .status     = IBV_WC_RESP_TIMEOUT_ERR,       /* 20 */
        .name       = "IBV_WC_RESP_TIMEOUT_ERR",
        .meaning_cn = "响应超时错误",
        .cause1     = "远端未在超时时间内响应",
        .cause2     = "网络拥塞导致延迟过高",
        .cause3     = "QP 的 timeout 参数设置过小",
        .fix        = "增大 timeout 值; 检查网络延迟; 检查对端状态",
    },
    {
        .status     = IBV_WC_GENERAL_ERR,            /* 21 */
        .name       = "IBV_WC_GENERAL_ERR",
        .meaning_cn = "通用错误",
        .cause1     = "其他未分类的错误",
        .cause2     = "传输层检测到未知问题",
        .cause3     = NULL,
        .fix        = "检查 vendor_err 字段获取更多信息; 查看 dmesg",
    },
    {
        .status     = IBV_WC_TM_ERR,                 /* 22 */
        .name       = "IBV_WC_TM_ERR",
        .meaning_cn = "Tag Matching 错误",
        .cause1     = "Tag Matching 操作失败",
        .cause2     = "TM 参数无效",
        .cause3     = NULL,
        .fix        = "检查 Tag Matching 配置",
    },
    {
        .status     = IBV_WC_TM_RNDV_INCOMPLETE,     /* 23 */
        .name       = "IBV_WC_TM_RNDV_INCOMPLETE",
        .meaning_cn = "Tag Matching Rendezvous 未完成",
        .cause1     = "TM Rendezvous 协议中断",
        .cause2     = NULL,
        .cause3     = NULL,
        .fix        = "检查 TM Rendezvous 流程",
    },
};

static const int NUM_WC_ERRORS = sizeof(wc_errors) / sizeof(wc_errors[0]);

/* ========== 异步事件信息 ========== */
struct async_event_info {
    int         type;
    const char *name;
    const char *meaning_cn;
    const char *level;      /* "设备级" / "端口级" / "QP级" */
};

static const struct async_event_info async_events[] = {
    { IBV_EVENT_CQ_ERR,             "IBV_EVENT_CQ_ERR",
      "CQ 溢出错误 (CQ 已满但仍有新 CQE)", "CQ级" },
    { IBV_EVENT_QP_FATAL,           "IBV_EVENT_QP_FATAL",
      "QP 致命错误 (QP 进入 ERROR 状态)", "QP级" },
    { IBV_EVENT_QP_REQ_ERR,         "IBV_EVENT_QP_REQ_ERR",
      "QP 请求错误 (发送方检测到错误)", "QP级" },
    { IBV_EVENT_QP_ACCESS_ERR,      "IBV_EVENT_QP_ACCESS_ERR",
      "QP 访问错误 (接收方检测到访问违规)", "QP级" },
    { IBV_EVENT_COMM_EST,           "IBV_EVENT_COMM_EST",
      "通信建立 (QP 首次收到请求)", "QP级" },
    { IBV_EVENT_SQ_DRAINED,         "IBV_EVENT_SQ_DRAINED",
      "SQ 已排空 (SQD 状态下所有 WR 已完成)", "QP级" },
    { IBV_EVENT_PATH_MIG,           "IBV_EVENT_PATH_MIG",
      "路径迁移完成 (备用路径已激活)", "QP级" },
    { IBV_EVENT_PATH_MIG_ERR,       "IBV_EVENT_PATH_MIG_ERR",
      "路径迁移失败", "QP级" },
    { IBV_EVENT_DEVICE_FATAL,       "IBV_EVENT_DEVICE_FATAL",
      "设备致命错误 (需要重置设备)", "设备级" },
    { IBV_EVENT_PORT_ACTIVE,        "IBV_EVENT_PORT_ACTIVE",
      "端口激活 (链路变为 ACTIVE)", "端口级" },
    { IBV_EVENT_PORT_ERR,           "IBV_EVENT_PORT_ERR",
      "端口错误 (链路变为 DOWN)", "端口级" },
    { IBV_EVENT_LID_CHANGE,         "IBV_EVENT_LID_CHANGE",
      "LID 变更 (SM 重新分配了 LID)", "端口级" },
    { IBV_EVENT_PKEY_CHANGE,        "IBV_EVENT_PKEY_CHANGE",
      "P_Key 表变更", "端口级" },
    { IBV_EVENT_SM_CHANGE,          "IBV_EVENT_SM_CHANGE",
      "SM (Subnet Manager) 变更", "端口级" },
    { IBV_EVENT_SRQ_ERR,            "IBV_EVENT_SRQ_ERR",
      "SRQ 错误", "SRQ级" },
    { IBV_EVENT_SRQ_LIMIT_REACHED,  "IBV_EVENT_SRQ_LIMIT_REACHED",
      "SRQ 水位线到达 (recv WR 数低于阈值)", "SRQ级" },
    { IBV_EVENT_QP_LAST_WQE_REACHED,"IBV_EVENT_QP_LAST_WQE_REACHED",
      "QP 最后一个 WQE 已到达 (SRQ 关联的 QP)", "QP级" },
    { IBV_EVENT_CLIENT_REREGISTER,  "IBV_EVENT_CLIENT_REREGISTER",
      "客户端需要重新注册 (SM 请求)", "端口级" },
    { IBV_EVENT_GID_CHANGE,         "IBV_EVENT_GID_CHANGE",
      "GID 表变更 (网络配置变化)", "端口级" },
};

static const int NUM_ASYNC_EVENTS = sizeof(async_events) / sizeof(async_events[0]);

/* ========== 打印单个 WC 错误详情 ========== */
static void print_wc_error_detail(const struct wc_error_info *info)
{
    printf("\n");
    printf("  " C_BOLD "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" C_NC "\n");
    printf("  " C_BOLD "状态码: %d" C_NC "\n", info->status);
    printf("  " C_CYAN "名称:   %s" C_NC "\n", info->name);
    printf("  " C_YELLOW "含义:   %s" C_NC "\n", info->meaning_cn);

    if (info->cause1 || info->cause2 || info->cause3) {
        printf("  " C_RED "常见原因:" C_NC "\n");
        if (info->cause1) printf("    1. %s\n", info->cause1);
        if (info->cause2) printf("    2. %s\n", info->cause2);
        if (info->cause3) printf("    3. %s\n", info->cause3);
    }

    printf("  " C_GREEN "修复建议:" C_NC "\n");
    printf("    %s\n", info->fix);
}

/* ========== 打印所有 WC 错误码 ========== */
static void print_all_wc_errors(void)
{
    int i;

    printf(C_BOLD C_CYAN "\n╔═══════════════════════════════════════════════════╗\n");
    printf("║        RDMA Work Completion 错误码大全            ║\n");
    printf("╚═══════════════════════════════════════════════════╝\n" C_NC);

    /* 摘要表 */
    printf("\n  " C_BOLD "%-6s %-32s %s" C_NC "\n", "编号", "名称", "含义");
    printf("  %-6s %-32s %s\n",
           "------", "--------------------------------", "----------");

    for (i = 0; i < NUM_WC_ERRORS; i++) {
        const char *color = (wc_errors[i].status == 0) ? C_GREEN : C_RED;
        printf("  %s%-6d %-32s %s" C_NC "\n",
               color, wc_errors[i].status,
               wc_errors[i].name, wc_errors[i].meaning_cn);
    }

    /* 详细说明 */
    printf(C_BOLD "\n━━━ 详细说明 ━━━" C_NC "\n");
    for (i = 0; i < NUM_WC_ERRORS; i++) {
        print_wc_error_detail(&wc_errors[i]);
    }
}

/* ========== 打印所有异步事件 ========== */
static void print_all_async_events(void)
{
    int i;

    printf(C_BOLD C_CYAN "\n╔═══════════════════════════════════════════════════╗\n");
    printf("║          RDMA 异步事件码大全                      ║\n");
    printf("╚═══════════════════════════════════════════════════╝\n" C_NC);

    printf("\n  " C_BOLD "%-6s %-8s %-38s %s" C_NC "\n",
           "编号", "级别", "名称", "含义");
    printf("  %-6s %-8s %-38s %s\n",
           "------", "--------", "--------------------------------------", "----------");

    for (i = 0; i < NUM_ASYNC_EVENTS; i++) {
        printf("  %-6d %-8s %-38s %s\n",
               async_events[i].type,
               async_events[i].level,
               async_events[i].name,
               async_events[i].meaning_cn);
    }

    printf("\n  " C_BOLD "异步事件处理方式:" C_NC "\n");
    printf("    1. 创建专门的事件处理线程\n");
    printf("    2. 调用 ibv_get_async_event() 阻塞等待事件\n");
    printf("    3. 根据事件类型采取恢复措施\n");
    printf("    4. 调用 ibv_ack_async_event() 确认事件\n");
    printf("\n  " C_BOLD "示例代码:" C_NC "\n");
    printf("    struct ibv_async_event event;\n");
    printf("    ibv_get_async_event(ctx, &event);\n");
    printf("    printf(\"事件: %%d\\n\", event.event_type);\n");
    printf("    ibv_ack_async_event(&event);\n");
}

/* ========== 查询特定错误码 ========== */
static void query_error_code(int code)
{
    int i;

    /* 先在 WC 错误码中查找 */
    for (i = 0; i < NUM_WC_ERRORS; i++) {
        if (wc_errors[i].status == code) {
            printf(C_BOLD "\n查询结果: WC 状态码 %d\n" C_NC, code);
            print_wc_error_detail(&wc_errors[i]);

            /* 额外: 使用 ibv_wc_status_str 验证 */
            printf("\n  ibv_wc_status_str(%d) = \"%s\"\n",
                   code, ibv_wc_status_str(code));
            return;
        }
    }

    /* 再在异步事件中查找 */
    for (i = 0; i < NUM_ASYNC_EVENTS; i++) {
        if (async_events[i].type == code) {
            printf(C_BOLD "\n查询结果: 异步事件码 %d\n" C_NC, code);
            printf("  名称: %s\n", async_events[i].name);
            printf("  级别: %s\n", async_events[i].level);
            printf("  含义: %s\n", async_events[i].meaning_cn);
            return;
        }
    }

    printf(C_RED "\n未找到错误码 %d 的信息\n" C_NC, code);
    printf("  WC 错误码范围: 0-%d\n", NUM_WC_ERRORS - 1);
    printf("  提示: 使用 ./error_diagnosis 查看所有支持的错误码\n");
}

/* ========== 主函数 ========== */
int main(int argc, char *argv[])
{
    printf("=== RDMA 错误码诊断工具 ===\n");

    if (argc == 1) {
        /* 无参数: 打印所有错误码 */
        print_all_wc_errors();
        print_all_async_events();

        printf(C_BOLD C_CYAN "\n━━━ 使用提示 ━━━\n" C_NC);
        printf("  查询特定错误码: ./error_diagnosis <status_code>\n");
        printf("  例: ./error_diagnosis 5    → 查询 IBV_WC_WR_FLUSH_ERR\n");
        printf("  例: ./error_diagnosis 13   → 查询 IBV_WC_RNR_RETRY_EXC_ERR\n");
    } else {
        /* 有参数: 查询特定错误码 */
        int code = atoi(argv[1]);
        query_error_code(code);
    }

    printf("\n");
    return 0;
}
