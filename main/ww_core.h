// main/ww_core.h —— 狼人杀规则状态机(纯 C,零 ESP-IDF / LVGL 依赖)。
//
// 同一份代码用在三处:固件、主机单测 tests/test_ww_core.c、主机仿真服务器
// sim/server.c。时间(毫秒)和随机种子一律从外面传进来,所以测试可以
// 精确地"快进"时间、复现随机结果。
//
// 线程模型:本模块不加锁。固件里由 ww_app.c 用一把互斥锁包住所有调用。
//
// 规则以 ~/cc/werewolf 的 game.py 为准,差异见 HANDOFF.md「规则取舍」。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WW_MIN_SEATS    2
#define WW_MAX_SEATS    12
#define WW_MAX_PLAYERS  16      // 含还没入座的人
#define WW_TOKEN_LEN    16
#define WW_NAME_MAX     36      // 字节(UTF-8),约 12 个汉字
#define WW_NOTICE_MAX   5       // 每个座位保留最近几条私人消息
#define WW_NOTICE_LEN   96
#define WW_LOG_MAX      10      // 公共公告保留条数
#define WW_LOG_LEN      200
#define WW_CUE_QUEUE    8
#define WW_SPEECH_MAX   12      // 发言记录条数(环形,跨天保留,给 AI 当上下文)
#define WW_SPEECH_LEN   400     // 每条字节数(UTF-8,约 130 个汉字)
#define WW_T_BRAIN      25000   // 有 AI 大脑时,机器人等大脑决策这么久,超时就随机兜底

// 节奏(毫秒)。没有语音主持,阶段之间靠这些计时推进。
#define WW_T_DEAL         10000   // 发身份,看手机
#define WW_T_NIGHT         4000   // 天黑请闭眼
// 夜里每个角色"睁眼"阶段至少持续一段随机时长(8–15 秒):AI 1–2 秒就能行动完、真人要
// 十几秒,固定 4 秒下限会让大家从节奏听出"这个身份是 AI / 已经死了"(真机试玩发现)。
#define WW_T_ROLE_MIN_LO   8000
#define WW_T_ROLE_MIN      15000  // 随机下限的上界(测试里"等满下限"用它)
#define WW_T_ROLE_TIMEOUT 60000   // 夜间行动超时兜底
#define WW_T_FAKE_MIN      8000   // 角色已死/不存在时的假等待,和真阶段落在同一区间
#define WW_T_FAKE_MAX      18000
#define WW_T_CLOSE         2500   // "xx请闭眼"间隔
#define WW_T_DAWN          6000   // 天亮公布死讯
#define WW_T_VOTE_MIN      3000   // 投票最短时长
#define WW_T_BOT_VOTE_LO   3000   // 机器人/AI 投票前随机"想一会儿"(不然一开投就齐刷刷投完)
#define WW_T_BOT_VOTE_HI   15000
#define WW_T_RESULT        8000   // 放逐结果展示
#define WW_T_ONLINE        6000   // 多久没轮询算离线

typedef enum {
    WW_ROLE_NONE = 0,
    WW_ROLE_WOLF,
    WW_ROLE_SEER,
    WW_ROLE_WITCH,
    WW_ROLE_VILLAGER,
} ww_role_t;

typedef enum {
    WW_PH_LOBBY = 0,
    WW_PH_DEAL,         // 发身份
    WW_PH_NIGHT,        // 天黑请闭眼
    WW_PH_WOLF,
    WW_PH_WITCH,
    WW_PH_SEER,
    WW_PH_DAWN,         // 天亮公布死讯
    WW_PH_DISCUSS,
    WW_PH_VOTE,
    WW_PH_RESULT,       // 放逐结果
    WW_PH_OVER,
} ww_phase_t;

typedef enum { WW_WIN_NONE = 0, WW_WIN_GOOD, WW_WIN_WOLF } ww_winner_t;

// 提示音/语音事件。核心只负责"该响了",放什么声音由固件决定。
typedef enum {
    WW_CUE_NONE = 0,
    WW_CUE_DEAL,          // 身份已发放,请查看手机
    WW_CUE_NIGHT,         // 天黑请闭眼
    WW_CUE_WOLF_OPEN,
    WW_CUE_WOLF_CLOSE,
    WW_CUE_WITCH_OPEN,
    WW_CUE_WITCH_CLOSE,
    WW_CUE_SEER_OPEN,
    WW_CUE_SEER_CLOSE,
    WW_CUE_DAWN_PEACE,    // 天亮了,昨晚是平安夜
    WW_CUE_DAWN_DEATH,    // 天亮了,昨晚有玩家死亡
    WW_CUE_DISCUSS,       // 请按顺序发言
    WW_CUE_VOTE,          // 开始投票
    WW_CUE_VOTE_END,      // 投票结束
    WW_CUE_WIN_GOOD,
    WW_CUE_WIN_WOLF,
    WW_CUE_COUNT,
} ww_cue_t;

// 动作返回码
enum {
    WW_OK = 0,
    WW_E_TOKEN = -1,    // 不认识这个 token(被踢了/设备重启过)
    WW_E_PHASE = -2,    // 现在不能做这个
    WW_E_ARG = -3,      // 目标无效
    WW_E_TAKEN = -4,    // 座位已被占用
    WW_E_FULL = -5,     // 房间已满
    WW_E_NAME = -6,     // 名字为空
    WW_E_DONE = -7,     // 已经提交过了
    WW_E_ROLE = -8,     // 身份不符
    WW_E_DEAD = -9,     // 已出局
    WW_E_SEATS = -10,   // 没坐满
};

typedef struct {
    bool used;
    char token[WW_TOKEN_LEN + 1];
    char name[WW_NAME_MAX + 1];
    uint8_t seat;           // 0 = 未入座
    uint8_t role;           // ww_role_t
    bool alive;
    uint32_t last_seen;     // ms
    bool bot;               // 测试用机器人:随机行动,永远在线
    uint32_t bot_at;        // 机器人下一次行动的时刻(0 = 还没排)
    uint32_t ai_after;      // AI 投票最早的时刻(随机 3–15 秒,像人在犹豫;0 = 不限)
} ww_player_t;

typedef struct {
    uint8_t seat;
    uint8_t round;
    char text[WW_SPEECH_LEN];
} ww_speech_t;

// 设备正在忙什么(手机和主持屏上显示"3号正在说话…"之类)
typedef enum {
    WW_BUSY_NONE = 0,
    WW_BUSY_REC,        // 按住确定录音中
    WW_BUSY_ASR,        // 录音转文字中
    WW_BUSY_THINK,      // AI 在想发言
    WW_BUSY_SPEAK,      // AI 在说话
    WW_BUSY_CONNECT,    // 按住确定后正在连语音识别,听到"嘀"再说
} ww_busy_t;

typedef struct {
    ww_phase_t phase;
    uint8_t step;           // 夜晚角色阶段:0 = 睁眼行动中,1 = 闭眼间隔
    bool fake;              // 本角色阶段是假等待(角色已死/没有该角色/没药了)
    uint32_t phase_start;   // 进入当前 phase/step 的时刻
    uint32_t until;         // 自动推进的时刻(0 = 不自动)
    uint32_t min_until;     // 行动完成后也要等到这时
    uint32_t deadline;      // 夜间行动超时

    uint8_t round;          // 第几夜/天
    uint8_t seat_count;
    ww_player_t players[WW_MAX_PLAYERS];

    // 夜晚
    int8_t wolf_votes[WW_MAX_PLAYERS];     // -1 未投;0 空刀;n 座位
    uint8_t kill_seat;
    int8_t seer_target;                    // -1 未查
    int8_t witch_act;                      // -1 未选;0 不用;1 救;2 毒
    uint8_t witch_target;
    bool antidote;
    bool poison;
    bool saved;
    uint8_t poisoned;
    uint8_t dead_tonight[WW_MAX_SEATS];
    uint8_t n_dead;

    // 白天
    int8_t day_votes[WW_MAX_PLAYERS];      // -1 未投;0 弃票;n 座位
    uint8_t order[WW_MAX_SEATS];           // 当天发言顺序
    uint8_t n_order;
    uint8_t speaker;                       // order 下标
    uint32_t speaker_since;
    uint8_t speech_start;                  // 上一轮首个发言者
    uint8_t out_seat;                      // 本轮放逐

    ww_winner_t winner;

    char log[WW_LOG_MAX][WW_LOG_LEN];      // 环形
    uint16_t log_total;
    char notices[WW_MAX_SEATS + 1][WW_NOTICE_MAX][WW_NOTICE_LEN];  // 按座位
    uint16_t notice_total[WW_MAX_SEATS + 1];

    ww_speech_t speech[WW_SPEECH_MAX];     // 环形
    uint16_t speech_total;
    uint16_t spoke_mask;                   // 本轮讨论已发过言的座位(bit = 座位号)
    bool brain;                            // 有 AI 大脑(配好了 Key):机器人等大脑,不再立刻随机
    uint8_t busy;                          // ww_busy_t
    uint8_t busy_seat;

    uint8_t cues[WW_CUE_QUEUE];
    uint8_t cue_head, cue_tail;

    uint16_t online_mask;   // 上次 tick 时各座位在线情况,变化时 bump 版本
    uint32_t version;       // 任何可见变化都 +1;轮询用它判断"有没有新东西"
    uint32_t rng;
    uint32_t now;           // 最近一次 ww_tick 的时刻(给 ww_ai 判断 AI 什么时候能出手)
} ww_game_t;

// ---- 生命周期 ----
void ww_init(ww_game_t *g, uint32_t seed);
void ww_reseed(ww_game_t *g, uint32_t entropy);   // 混入外部熵(设备上的硬件随机数)
void ww_tick(ww_game_t *g, uint32_t now);         // 推进计时,建议 100ms 一次

// ---- 玩家(手机)----
// 加入房间,成功返回玩家下标并把 token 写进 out_token(WW_TOKEN_LEN+1)。
int ww_join(ww_game_t *g, const char *name, uint32_t now, char *out_token);
int ww_find(const ww_game_t *g, const char *token);          // 找不到返回 -1
void ww_touch(ww_game_t *g, int pidx, uint32_t now);         // 记录"在线"

typedef enum {
    WW_ACT_SEAT = 0,    // arg = 座位
    WW_ACT_UNSEAT,
    WW_ACT_RENAME,      // text = 新名字
    WW_ACT_LEAVE,
    WW_ACT_WOLF,        // arg = 座位,0 = 空刀
    WW_ACT_WITCH,       // arg = 0 不用 / 1 救 / 2 毒;arg2 = 目标
    WW_ACT_SEER,        // arg = 座位
    WW_ACT_START_VOTE,
    WW_ACT_VOTE,        // arg = 座位,0 = 弃票
} ww_act_t;

int ww_act(ww_game_t *g, int pidx, ww_act_t act, int arg, int arg2,
           const char *text, uint32_t now);

// ---- 主持人(FoloToy 按键)----
int ww_set_seats(ww_game_t *g, int n);             // 返回生效后的座位数;减座位时高号玩家挪到空着的低号
int ww_add_bots(ww_game_t *g, uint32_t now);       // 大厅里把空座位补满机器人,返回加了几个
int ww_bot_count(const ww_game_t *g);
bool ww_is_bot(const ww_game_t *g, int seat);

// ---- 发言记录 / AI 大脑 ----
// 记一条发言。advance = 说完自动轮到下一位(AI 发言用;真人由主持人按"下")。
void ww_add_speech(ww_game_t *g, int seat, const char *text, bool advance, uint32_t now);
const ww_speech_t *ww_speech_at(const ww_game_t *g, int back);   // back=0 最新,没有返回 NULL
bool ww_has_spoken(const ww_game_t *g, int seat);                // 本轮讨论
int ww_current_speaker(const ww_game_t *g);                      // 讨论阶段当前发言座位,否则 0
void ww_set_busy(ww_game_t *g, ww_busy_t busy, int seat);
uint32_t ww_rand(ww_game_t *g);                               // 状态机自己的随机数(AI 兜底用)
int ww_min_seats(const ww_game_t *g);              // 当前允许的最少座位数
int ww_start(ww_game_t *g, uint32_t now);
bool ww_force(ww_game_t *g, uint32_t now);         // 强制下一步/跳过等待
void ww_speaker_move(ww_game_t *g, int delta, uint32_t now);
int ww_kick(ww_game_t *g, int seat);               // 仅大厅
void ww_kick_all(ww_game_t *g);                    // 清空房间,回大厅
void ww_reset(ww_game_t *g);                       // 结束本局回大厅,保留座位

// ---- 查询 ----
int ww_seated_count(const ww_game_t *g);
int ww_alive_count(const ww_game_t *g);
int ww_by_seat(const ww_game_t *g, int seat);      // 玩家下标或 -1
bool ww_is_online(const ww_game_t *g, int pidx, uint32_t now);
bool ww_is_pending(const ww_game_t *g, int pidx);  // 该玩家当前是否还欠一个操作
int ww_voted_count(const ww_game_t *g);
int ww_pending_count(const ww_game_t *g);          // 投票阶段还没投的存活人数
bool ww_night_role_phase(ww_phase_t ph);
uint32_t ww_elapsed_ms(const ww_game_t *g, uint32_t now);
int ww_left_s(const ww_game_t *g, uint32_t now);   // 自动推进倒计时(秒),没有返回 -1
int ww_role_count(int n, ww_role_t role);          // 板子:n 人局里某角色几个
const char *ww_role_name(int role);
const char *ww_phase_key(ww_phase_t ph);           // 给网页用的英文键
const char *ww_err_str(int code);
const char *ww_log_at(const ww_game_t *g, int back);  // back=0 最新一条,不存在返回 NULL
ww_cue_t ww_pop_cue(ww_game_t *g);

// 按观察者生成状态 JSON。pidx = -1 表示主持屏(不含任何私密信息)。
// 返回写入的字节数;缓冲不够返回 0。
size_t ww_state_json(const ww_game_t *g, int pidx, uint32_t now, char *buf, size_t cap);
