/**
 * RDMA WC 错误码速查工具 (纯 printf 版本，无需 RDMA 头文件)
 *
 * 功能:
 *   - 不带参数: 打印所有常见 WC 状态码的详细说明
 *   - 带参数:   查询指定错误码 (支持数值和名称)
 *
 * 用法:
 *   ./error_cheatsheet                          # 打印全部错误码
 *   ./error_cheatsheet 12                       # 查询指定错误码 (数值)
 *   ./error_cheatsheet IBV_WC_RETRY_EXC_ERR     # 查询指定错误 (名称)
 *   ./error_cheatsheet RETRY_EXC_ERR            # 支持简写 (省略 IBV_WC_ 前缀)
 *   ./error_cheatsheet RETRY                    # 模糊搜索
 *
 * 每个错误包含:
 *   - 错误码数值和枚举名称 (IBV_WC_*)
 *   - 中文含义/描述
 *   - 最常见的 3 个原因
 *   - 修复建议
 *
 * 编译 (无需 libibverbs):
 *   gcc -Wall -O2 -g -o error_cheatsheet error_cheatsheet.c
 */

#include <stdio.h>      /* printf, fprintf */
#include <stdlib.h>     /* strtol */
#include <string.h>     /* strcmp, strcasecmp, strcasestr, strlen */

/* ========== 终端颜色定义 ========== */
#define CLR_RED     "\033[0;31m"    /* 红色 (错误) */
#define CLR_GREEN   "\033[0;32m"    /* 绿色 (成功) */
#define CLR_YELLOW  "\033[1;33m"    /* 黄色 (原因) */
#define CLR_BLUE    "\033[0;34m"    /* 蓝色 (修复) */
#define CLR_CYAN    "\033[0;36m"    /* 青色 (标题) */
#define CLR_BOLD    "\033[1m"       /* 粗体 */
#define CLR_RESET   "\033[0m"       /* 重置 */

/* ========== 错误条目结构体 ========== */
struct error_entry {
    int         code;           /* 状态码数值 */
    const char *name;           /* 枚举名称 (IBV_WC_*) */
    const char *meaning_cn;     /* 中文含义 */
    const char *cause1;         /* 常见原因 1 */
    const char *cause2;         /* 常见原因 2 */
    const char *cause3;         /* 常见原因 3 */
    const char *fix;            /* 修复建议 */
};

/* ========== 所有 WC 状态码定义 (硬编码，无需 infiniband/verbs.h) ========== */
static const struct error_entry errors[] = {
    /* --- 0: 成功 --- */
    {
        .code       = 0,
        .name       = "IBV_WC_SUCCESS",
        .meaning_cn = "操作成功完成",
        .cause1     = "这不是错误，表示 WR 已被 HCA 成功处理",
        .cause2     = "Send/Recv/RDMA Read/Write/Atomic 均正常完成",
        .cause3     = "数据已可靠地发送或接收到目标缓冲区",
        .fix        = "无需修复。检查 wc.byte_len (接收侧) 和 wc.opcode 获取详情。"
    },
    /* --- 1: 本地长度错误 --- */
    {
        .code       = 1,
        .name       = "IBV_WC_LOC_LEN_ERR",
        .meaning_cn = "本地长度错误 — 数据长度超出限制",
        .cause1     = "发送数据超过 QP 的 max_inline_data 或 path MTU 限制",
        .cause2     = "接收缓冲区 (Recv WR 的 SGE) 太小，装不下收到的数据",
        .cause3     = "SGE 的 length 字段设置不正确 (为 0 或超出 MR 范围)",
        .fix        = "检查 SGE.length 值。Recv 缓冲区应 >= 发送数据大小。确认 path_mtu 设置。"
    },
    /* --- 2: 本地 QP 操作错误 --- */
    {
        .code       = 2,
        .name       = "IBV_WC_LOC_QP_OP_ERR",
        .meaning_cn = "本地 QP 操作错误 — QP 配置或状态不正确",
        .cause1     = "QP 未转换到 RTS 状态就执行了 ibv_post_send()",
        .cause2     = "QP 类型不支持请求的操作 (如 UD QP 执行 RDMA Write)",
        .cause3     = "发送操作的参数不合法 (如 num_sge=0、opcode 无效)",
        .fix        = "确保 QP 在 RTS 状态再发送。用 ibv_query_qp() 检查 QP 状态。"
    },
    /* --- 3: 本地 EEC 操作错误 (RD QP，极少见) --- */
    {
        .code       = 3,
        .name       = "IBV_WC_LOC_EEC_OP_ERR",
        .meaning_cn = "本地 EEC 操作错误 (RD QP 相关，极少见)",
        .cause1     = "EEC (End-to-End Context) 状态不正确",
        .cause2     = "仅在 RD (Reliable Datagram) QP 类型中出现",
        .cause3     = "RD QP 极少使用，大多数环境不会遇到此错误",
        .fix        = "如果你使用的是 RC QP，不应该看到此错误。检查 QP 类型。"
    },
    /* --- 4: 本地保护错误 --- */
    {
        .code       = 4,
        .name       = "IBV_WC_LOC_PROT_ERR",
        .meaning_cn = "本地保护错误 — 内存访问权限不匹配",
        .cause1     = "MR 注册时未包含 IBV_ACCESS_LOCAL_WRITE 但 NIC 需要写入本地",
        .cause2     = "SGE 的 lkey 与实际 MR 不匹配 (使用了错误的 lkey)",
        .cause3     = "访问的内存地址超出了 MR 注册的地址范围",
        .fix        = "检查 ibv_reg_mr() 的 access_flags。接收操作必须有 LOCAL_WRITE。"
    },
    /* --- 5: WR 冲刷 --- */
    {
        .code       = 5,
        .name       = "IBV_WC_WR_FLUSH_ERR",
        .meaning_cn = "WR 被冲刷 — QP 已进入 Error 状态",
        .cause1     = "之前的某个操作出错，导致 QP 转入 Error 状态",
        .cause2     = "Error 状态下所有未完成的 WR 都会被冲刷为此错误",
        .cause3     = "对端断开连接或 QP 被 ibv_modify_qp 手动置为 Error",
        .fix        = "★ FLUSH_ERR 不是根因! 找它之前的第一个非 FLUSH 错误。重置 QP 或重建连接。"
    },
    /* --- 6: MW 绑定错误 --- */
    {
        .code       = 6,
        .name       = "IBV_WC_MW_BIND_ERR",
        .meaning_cn = "Memory Window 绑定错误",
        .cause1     = "MW 绑定操作的参数不合法",
        .cause2     = "MR 不支持 MW 绑定 (注册时未指定 MW 绑定标志)",
        .cause3     = "Memory Window 功能极少使用",
        .fix        = "大多数程序不使用 MW。如使用，检查 MR 注册和 MW 绑定参数。"
    },
    /* --- 7: 坏响应 --- */
    {
        .code       = 7,
        .name       = "IBV_WC_BAD_RESP_ERR",
        .meaning_cn = "收到意外/损坏的响应包",
        .cause1     = "响应包与期望的操作类型不匹配",
        .cause2     = "网络中存在数据损坏 (CRC 校验失败等)",
        .cause3     = "对端 HCA 固件 bug 或硬件故障",
        .fix        = "检查网络链路质量 (ibstatus, perfquery)。可能需要更换网线/光纤。"
    },
    /* --- 8: 本地访问错误 --- */
    {
        .code       = 8,
        .name       = "IBV_WC_LOC_ACCESS_ERR",
        .meaning_cn = "本地访问错误 — lkey 无效或 MR 已注销",
        .cause1     = "使用了已被 ibv_dereg_mr() 注销的 MR 的 lkey",
        .cause2     = "lkey 值错误 (打字错误或使用了其他 MR 的 lkey)",
        .cause3     = "MR 注册的地址范围不包含 SGE 指定的地址区间",
        .fix        = "确认 SGE.lkey 对应正确的、未注销的 MR。检查地址范围。"
    },
    /* --- 9: 远端无效请求 --- */
    {
        .code       = 9,
        .name       = "IBV_WC_REM_INV_REQ_ERR",
        .meaning_cn = "远端无效请求 — 对端认为请求不合法",
        .cause1     = "RDMA Write/Read 的目标地址超出对端 MR 注册范围",
        .cause2     = "请求的操作类型对端 QP 不支持 (如 Atomic 但对端未启用)",
        .cause3     = "建连参数错误导致请求被误路由 (dest_qp_num 不匹配)",
        .fix        = "打印两端的 rdma_endpoint 信息逐项对比。检查 remote_addr + length 范围。"
    },
    /* --- 10: 远端访问错误 --- */
    {
        .code       = 10,
        .name       = "IBV_WC_REM_ACCESS_ERR",
        .meaning_cn = "远端访问错误 — 对端 MR 权限不足",
        .cause1     = "RDMA Write 但对端 MR 未设置 IBV_ACCESS_REMOTE_WRITE",
        .cause2     = "RDMA Read 但对端 MR 未设置 IBV_ACCESS_REMOTE_READ",
        .cause3     = "Atomic 但对端 MR 未设 REMOTE_ATOMIC; 或 rkey 不匹配/已失效",
        .fix        = "确保对端 ibv_reg_mr() 包含对应的 REMOTE_* 权限。检查 rkey 交换。"
    },
    /* --- 11: 远端操作错误 --- */
    {
        .code       = 11,
        .name       = "IBV_WC_REM_OP_ERR",
        .meaning_cn = "远端操作错误 — 对端内部处理失败",
        .cause1     = "对端 HCA 处理请求时遇到内部错误",
        .cause2     = "对端 QP 已进入 Error 状态",
        .cause3     = "对端资源不足 (如 SRQ 溢出、内存映射失效)",
        .fix        = "检查对端的 dmesg 和 QP 状态。对端程序可能已 crash。"
    },
    /* --- 12: 重传超限 --- */
    {
        .code       = 12,
        .name       = "IBV_WC_RETRY_EXC_ERR",
        .meaning_cn = "重传次数超限 — 网络不通或对端不可达",
        .cause1     = "网络物理连接断开 (网线/光纤/交换机故障)",
        .cause2     = "对端程序已退出、机器已关机或 QP 已被销毁",
        .cause3     = "建连参数错误: dest_qp_num / LID / GID 不正确",
        .fix        = "1) ping 对端确认网络通 2) ibstatus 检查端口 3) 打印两端端点信息对比"
    },
    /* --- 13: RNR 重试超限 --- */
    {
        .code       = 13,
        .name       = "IBV_WC_RNR_RETRY_EXC_ERR",
        .meaning_cn = "RNR 重试超限 — 对端没有 Post Recv (接收队列为空)",
        .cause1     = "★ 最常见: 对端忘记 ibv_post_recv() 就被 Send 了数据",
        .cause2     = "对端 Recv WR 消耗完了，没有及时补充",
        .cause3     = "rnr_retry 设置为 0 (不重试)，应设为 7 (无限重试)",
        .fix        = "确保接收端先 Post Recv 再让发送端 Post Send。设置 rnr_retry=7。"
    },
    /* --- 14: 本地 RDD 违规 (RD QP，极少见) --- */
    {
        .code       = 14,
        .name       = "IBV_WC_LOC_RDD_VIOL_ERR",
        .meaning_cn = "本地 RDD 违规 (RD QP 相关，极少见)",
        .cause1     = "RD QP 的 Reliable Datagram Domain 不匹配",
        .cause2     = "仅在 RD QP 类型中出现",
        .cause3     = "绝大多数环境不支持 RD QP",
        .fix        = "不使用 RD QP 就不会遇到此错误。"
    },
    /* --- 15: 远端无效 RD 请求 --- */
    {
        .code       = 15,
        .name       = "IBV_WC_REM_INV_RD_REQ_ERR",
        .meaning_cn = "远端无效 RD 请求 (RD QP 相关，极少见)",
        .cause1     = "远端 RD 请求参数不合法",
        .cause2     = "仅在 RD QP 类型中出现",
        .cause3     = "绝大多数环境不支持 RD QP",
        .fix        = "不使用 RD QP 就不会遇到此错误。"
    },
    /* --- 19: 致命错误 --- */
    {
        .code       = 19,
        .name       = "IBV_WC_FATAL_ERR",
        .meaning_cn = "致命错误 — HCA 硬件故障",
        .cause1     = "HCA (网卡) 硬件内部不可恢复错误",
        .cause2     = "HCA 固件 (firmware) crash 或异常",
        .cause3     = "PCIe 总线错误或供电不稳定",
        .fix        = "检查 dmesg 查看硬件错误日志。尝试重启驱动。可能需要更换网卡。"
    },
    /* --- 20: 响应超时 --- */
    {
        .code       = 20,
        .name       = "IBV_WC_RESP_TIMEOUT_ERR",
        .meaning_cn = "响应超时 — 等待对端响应超时",
        .cause1     = "类似 RETRY_EXC_ERR，对端响应超时",
        .cause2     = "网络拥塞严重导致延迟超过超时阈值",
        .cause3     = "QP RTR→RTS 时的 timeout 参数设置过小",
        .fix        = "增大 timeout 参数 (如从 14 → 18)。检查网络延迟和丢包率。"
    },
};

/* 错误条目总数 */
#define NUM_ERRORS  (sizeof(errors) / sizeof(errors[0]))

/* ========== 打印单个错误条目 ========== */
static void print_error(const struct error_entry *e)
{
    /* 错误码名称和数值 (成功用绿色，错误用红色) */
    if (e->code == 0) {
        printf(CLR_GREEN CLR_BOLD "  [%2d] %s" CLR_RESET "\n",
               e->code, e->name);
    } else {
        printf(CLR_RED CLR_BOLD "  [%2d] %s" CLR_RESET "\n",
               e->code, e->name);
    }

    /* 中文含义 */
    printf("       含义: %s\n", e->meaning_cn);

    /* 三个常见原因 */
    printf(CLR_YELLOW);
    printf("       原因 1: %s\n", e->cause1);
    printf("       原因 2: %s\n", e->cause2);
    printf("       原因 3: %s\n", e->cause3);
    printf(CLR_RESET);

    /* 修复建议 */
    printf(CLR_BLUE "       修复: %s\n" CLR_RESET, e->fix);

    printf("\n");
}

/* ========== 打印所有错误码 ========== */
static void print_all_errors(void)
{
    printf("\n");
    printf(CLR_CYAN CLR_BOLD
           "============================================================\n"
           "  RDMA WC (Work Completion) 错误码速查表\n"
           "  共 %zu 种常见状态码 (纯查询版，无需 libibverbs)\n"
           "============================================================\n\n"
           CLR_RESET, NUM_ERRORS);

    /* 先打印最常遇到的 5 个错误 */
    printf(CLR_CYAN CLR_BOLD "--- 最常遇到的 5 个错误 ---\n\n" CLR_RESET);

    int common_codes[] = {12, 13, 10, 5, 4};   /* 按实际频率排列 */
    for (int i = 0; i < 5; i++) {
        for (size_t j = 0; j < NUM_ERRORS; j++) {
            if (errors[j].code == common_codes[i]) {
                print_error(&errors[j]);
                break;
            }
        }
    }

    /* 完整列表 */
    printf(CLR_CYAN CLR_BOLD "--- 完整错误码列表 ---\n\n" CLR_RESET);

    for (size_t i = 0; i < NUM_ERRORS; i++) {
        print_error(&errors[i]);
    }

    /* 调试技巧 */
    printf(CLR_CYAN
           "============================================================\n"
           "  调试技巧:\n"
           "    1. 代码中用 ibv_wc_status_str(wc.status) 获取英文描述\n"
           "    2. 用 print_wc_detail(&wc) 打印完整 WC 字段\n"
           "    3. WR_FLUSH_ERR (5) 不是根因，找它之前的第一个错误\n"
           "    4. wc.vendor_err 非零时包含厂商特定的错误细节\n"
           "    5. RETRY_EXC (12) 和 RNR_RETRY_EXC (13) 是最常见的两个错误\n"
           "============================================================\n"
           CLR_RESET "\n");
}

/* ========== 按数值或名称查询指定错误 ========== */
static int search_error(const char *query)
{
    /* 尝试解析为数值 */
    char *endptr;
    long code = strtol(query, &endptr, 10);
    if (*endptr == '\0') {
        /* 输入是数值 */
        for (size_t i = 0; i < NUM_ERRORS; i++) {
            if (errors[i].code == (int)code) {
                printf("\n");
                print_error(&errors[i]);
                return 0;
            }
        }
        fprintf(stderr, CLR_RED "未找到错误码: %ld\n" CLR_RESET, code);
        fprintf(stderr, "有效的错误码数值: 0~15, 19, 20\n");
        return 1;
    }

    /* 输入是名称 —— 支持完整名称和省略 IBV_WC_ 前缀的简写 */
    for (size_t i = 0; i < NUM_ERRORS; i++) {
        /* 完整名称匹配 (不区分大小写) */
        if (strcasecmp(query, errors[i].name) == 0) {
            printf("\n");
            print_error(&errors[i]);
            return 0;
        }

        /* 简写匹配: 省略 "IBV_WC_" 前缀 (7 个字符) */
        if (strlen(errors[i].name) > 7) {
            const char *short_name = errors[i].name + 7;
            if (strcasecmp(query, short_name) == 0) {
                printf("\n");
                print_error(&errors[i]);
                return 0;
            }
        }
    }

    /* 模糊匹配: 子串搜索 */
    int found = 0;
    for (size_t i = 0; i < NUM_ERRORS; i++) {
        if (strcasestr(errors[i].name, query) != NULL ||
            strstr(errors[i].meaning_cn, query) != NULL) {
            if (!found) {
                printf("\n" CLR_CYAN "模糊匹配结果:\n\n" CLR_RESET);
            }
            print_error(&errors[i]);
            found = 1;
        }
    }

    if (found) return 0;

    /* 没找到 */
    fprintf(stderr, CLR_RED "未找到匹配: %s\n" CLR_RESET, query);
    fprintf(stderr, "用法:\n");
    fprintf(stderr, "  ./error_cheatsheet                      # 打印全部\n");
    fprintf(stderr, "  ./error_cheatsheet 12                   # 数值查询\n");
    fprintf(stderr, "  ./error_cheatsheet IBV_WC_RETRY_EXC_ERR # 完整名称\n");
    fprintf(stderr, "  ./error_cheatsheet RETRY                # 模糊搜索\n");
    return 1;
}

/* ========== 主函数 ========== */
int main(int argc, char *argv[])
{
    if (argc == 1) {
        /* 无参数: 打印全部错误码 */
        print_all_errors();
        return 0;
    }

    if (argc == 2) {
        /* 有参数: 查询指定错误码 */
        return search_error(argv[1]);
    }

    /* 参数过多 */
    fprintf(stderr, "用法:\n");
    fprintf(stderr, "  %s                          # 打印全部错误码\n", argv[0]);
    fprintf(stderr, "  %s <错误名称或数值>          # 查询指定错误\n", argv[0]);
    fprintf(stderr, "\n");
    fprintf(stderr, "示例:\n");
    fprintf(stderr, "  %s 12                       # 按数值查询\n", argv[0]);
    fprintf(stderr, "  %s IBV_WC_REM_ACCESS_ERR    # 完整名称\n", argv[0]);
    fprintf(stderr, "  %s REM_ACCESS_ERR           # 省略 IBV_WC_ 前缀\n", argv[0]);
    fprintf(stderr, "  %s RETRY                    # 模糊搜索\n", argv[0]);
    return 1;
}
