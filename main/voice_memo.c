// main/voice_memo.c —— 碎碎念：三键实时语音记录 + 待办快捷记录。
//
// 按键约定(在本应用页面内)：
//   上/下 短按     移动选中行
//   上键 长按      录音暂停后 = 继续记录
//   下键 长按      列表页 = 删除选中项; 录音中 = 放弃; 时间页字段上 = 减 1
//   确定 短按      进入/播放/暂停/保存/加一(时间字段)
//   确定 长按      返回主页菜单(由 main.c 全局拦截)
//
// 存储: SPIFFS 分区 "store" 挂载在 /store。
//   /store/R<ts>.wav   16kHz 16bit 单声道 WAV 录音(滚动删除最旧,保证剩余空间)
//   /store/todo.txt    待办, 每行 "ts|done|text"
// 时间: 无 WiFi, 手动设置(NVS 记忆), 掉电后需重设; 上传同步留待 v2。
#include "demo.h"
#include "bsp_audio.h"
#include "bsp_display.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

LV_FONT_DECLARE(font_sui_16);

static const char *TAG = "voice_memo";

#define MOUNT_BASE     "/store"
#define REC_PREFIX     MOUNT_BASE "/R"
#define TODO_FILE      MOUNT_BASE "/todo.txt"
#define SAMPLE_RATE    16000
#define BYTES_PER_SEC  (SAMPLE_RATE * 2)          // 16bit 单声道
#define WAV_HEADER     44
#define CHUNK_SAMPLES  512
#define MAX_REC_SEC    180
#define KEEP_FREE_MIN  (600u * 1024u)             // 至少保留的空间,不足则滚动删最旧
#define VIS_ROWS       6
#define REC_MAX        24
#define TODO_MAX       64
#define TODO_TEXT_MAX  60

// ---------- 页面 ----------
typedef enum {
    PG_HOME = 0,   // 主页: 碎碎念 / 待办记录 / 时间设置
    PG_SUI,        // 碎碎念列表: 开始录音 + 录音列表
    PG_REC,        // 录音中
    PG_TODO,       // 待办: 新增 + 列表
    PG_TPL,        // 快捷待办模板
    PG_TIME,       // 时间设置
} page_t;

static page_t s_page = PG_HOME;
static int s_sel;                          // 当前行
static int s_scroll;                       // 列表窗口偏移

static lv_obj_t *s_scr;
static lv_obj_t *s_rows_box;               // 行容器
static lv_obj_t *s_footer;                 // 底部状态行
static lv_obj_t *s_rec_time;               // 录音页大计时

// ---------- 数据 ----------
typedef struct {
    uint32_t ts;
    uint32_t dur;
    uint32_t size;
    char name[48];
} rec_item_t;

typedef struct {
    uint32_t ts;
    bool done;
    char text[TODO_TEXT_MAX];
} todo_t;

static rec_item_t s_recs[REC_MAX];
static int s_rec_cnt;
static todo_t s_todos[TODO_MAX];
static int s_todo_cnt;

static const char *const TEMPLATES[] = {
    "巡检管网", "买材料", "交日报表", "开会", "回电话", "跟进维修", "其他事项",
};
#define TPL_COUNT (sizeof(TEMPLATES) / sizeof(TEMPLATES[0]))

// ---------- 时间 ----------
static uint32_t s_epoch_base;              // 基准 epoch(UTC 秒)
static int64_t  s_epoch_since_us;          // 基准建立时刻
static bool     s_time_set;

static uint32_t now_epoch(void) {
    if (!s_time_set) return 0;
    return s_epoch_base + (uint32_t)((esp_timer_get_time() - s_epoch_since_us) / 1000000LL);
}

static void time_store(uint32_t e) {
    s_epoch_base = e;
    s_epoch_since_us = esp_timer_get_time();
    s_time_set = true;
    nvs_handle_t h;
    if (nvs_open("vmemo", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "epoch", e);
        nvs_set_u64(h, "since", (uint64_t)s_epoch_since_us);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void time_load(void) {
    nvs_handle_t h;
    uint32_t e = 0;
    uint64_t since = 0;
    if (nvs_open("vmemo", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "epoch", &e);
        nvs_get_u64(h, "since", &since);
        nvs_close(h);
    }
    if (e > 0) {
        s_epoch_base = e;
        s_epoch_since_us = (int64_t)since;
        s_time_set = true;
    }
}

// "年月日时分"(本地) -> epoch(UTC)。简化 Howard Hinnant days_from_civil。
static uint32_t civil_to_epoch(int y, int m, int d, int hh, int mi) {
    y -= (m <= 2);
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = era * 146097L + (long)doe - 719468L;
    return (uint32_t)(days * 86400L + hh * 3600 + mi * 60) - 8u * 3600u;
}

static void epoch_to_local(uint32_t e, int *y, int *mo, int *d, int *hh, int *mi) {
    e += 8u * 3600u;
    long days = (long)(e / 86400u);
    uint32_t rem = e % 86400u;
    days += 719468;
    const long era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = (unsigned)(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long yy = (long)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned dd = doy - (153 * mp + 2) / 5 + 1;
    const unsigned mm = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yy + (mm <= 2));
    *mo = (int)mm;
    *d = (int)dd;
    *hh = (int)(rem / 3600u);
    *mi = (int)((rem % 3600u) / 60u);
}

// ---------- UI 基础 ----------
static lv_obj_t *mk_label(lv_obj_t *parent, const char *txt, lv_coord_t x, lv_coord_t y,
                          lv_coord_t w, int color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, &font_sui_16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, x, y);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    }
    return l;
}

// 页面重建:清空行容器并按当前页绘制
static void render(void);

static void build_page_shell(const char *title) {
    s_scr = ui_pixel_screen_create("");
    lv_obj_t *t = mk_label(s_scr, title, 0, 14, 161, UI_INK);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    s_rows_box = lv_obj_create(s_scr);
    lv_obj_remove_flag(s_rows_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_rows_box, 8, 48);
    lv_obj_set_size(s_rows_box, 224, 236);
    lv_obj_set_style_bg_opa(s_rows_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_rows_box, 0, 0);
    lv_obj_set_style_pad_all(s_rows_box, 0, 0);
    s_footer = mk_label(s_scr, "", 8, 290, 224, 0xFFFFFF);
}

static lv_obj_t *add_row(int idx, const char *txt, bool selected) {
    lv_obj_t *p = ui_pixel_panel_create(s_rows_box, 0, idx * 36, 224, 30, UI_PAPER);
    ui_pixel_set_selected(p, selected, true);
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, &font_sui_16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(UI_INK), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, 204);
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, 8, 5);
    return p;
}

static void clamp_sel(int max) {
    if (s_sel < 0) s_sel = 0;
    if (s_sel > max) s_sel = max;
    if (s_sel < s_scroll) s_scroll = s_sel;
    if (s_sel > s_scroll + VIS_ROWS - 1) s_scroll = s_sel - VIS_ROWS + 1;
    if (s_scroll < 0) s_scroll = 0;
}

// ---------- 存储 ----------
static int64_t spiffs_free(void) {
    size_t total = 0, used = 0;
    if (esp_spiffs_info("store", &total, &used) != ESP_OK) return -1;
    return (int64_t)total - (int64_t)used;
}

static void scan_recs(void) {
    s_rec_cnt = 0;
    DIR *dir = opendir(MOUNT_BASE);
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && s_rec_cnt < REC_MAX) {
        unsigned long ts;
        if (sscanf(ent->d_name, "R%lu.wav", &ts) != 1) continue;
        char path[80];
        snprintf(path, sizeof(path), MOUNT_BASE "/%s", ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || st.st_size <= WAV_HEADER) continue;
        rec_item_t *it = &s_recs[s_rec_cnt++];
        it->ts = (uint32_t)ts;
        it->size = (uint32_t)st.st_size;
        it->dur = (st.st_size - WAV_HEADER) / BYTES_PER_SEC;
        snprintf(it->name, sizeof(it->name), "%s", ent->d_name);
    }
    closedir(dir);
    for (int i = 0; i < s_rec_cnt - 1; i++)
        for (int j = 0; j < s_rec_cnt - 1 - i; j++)
            if (s_recs[j].ts < s_recs[j + 1].ts) {
                rec_item_t t = s_recs[j];
                s_recs[j] = s_recs[j + 1];
                s_recs[j + 1] = t;
            }
}

static void delete_rec(int idx) {
    if (idx < 0 || idx >= s_rec_cnt) return;
    char path[80];
    snprintf(path, sizeof(path), MOUNT_BASE "/%s", s_recs[idx].name);
    unlink(path);
    scan_recs();
}

static void ensure_space(void) {
    scan_recs();
    while (spiffs_free() < (int64_t)KEEP_FREE_MIN && s_rec_cnt > 0) {
        delete_rec(s_rec_cnt - 1);   // 最旧
    }
}

static void todos_load(void) {
    s_todo_cnt = 0;
    FILE *f = fopen(TODO_FILE, "r");
    if (!f) return;
    char line[TODO_TEXT_MAX + 32];
    while (fgets(line, sizeof(line), f) != NULL && s_todo_cnt < TODO_MAX) {
        line[strcspn(line, "\r\n")] = 0;
        char *p = strchr(line, '|');
        if (!p) continue;
        *p = 0;
        uint32_t ts = (uint32_t)strtoul(line, NULL, 10);
        p++;
        char *p2 = strchr(p, '|');
        if (!p2) continue;
        *p2 = 0;
        int done = atoi(p);
        p2++;
        if (strlen(p2) == 0) continue;
        todo_t *t = &s_todos[s_todo_cnt++];
        t->ts = ts;
        t->done = done != 0;
        snprintf(t->text, sizeof(t->text), "%s", p2);
    }
    fclose(f);
    for (int i = 0; i < s_todo_cnt - 1; i++)
        for (int j = 0; j < s_todo_cnt - 1 - i; j++)
            if (s_todos[j].ts < s_todos[j + 1].ts) {
                todo_t t = s_todos[j];
                s_todos[j] = s_todos[j + 1];
                s_todos[j + 1] = t;
            }
}

static void todos_save(void) {
    FILE *f = fopen(TODO_FILE, "w");
    if (!f) return;
    for (int i = 0; i < s_todo_cnt; i++) {
        fprintf(f, "%lu|%d|%s\n", (unsigned long)s_todos[i].ts, s_todos[i].done ? 1 : 0,
                s_todos[i].text);
    }
    fclose(f);
}

static void todo_add(const char *text) {
    if (s_todo_cnt >= TODO_MAX) {
        memmove(&s_todos[0], &s_todos[1], sizeof(todo_t) * (size_t)(TODO_MAX - 1));
        s_todo_cnt--;
    }
    todo_t *t = &s_todos[s_todo_cnt++];
    t->ts = now_epoch();
    t->done = false;
    snprintf(t->text, sizeof(t->text), "%s", text);
    todos_save();
}

// ---------- 音频工作线程 ----------
typedef enum {
    CMD_NONE = 0,
    CMD_REC_START,
    CMD_REC_PAUSE,
    CMD_REC_RESUME,
    CMD_REC_SAVE,
    CMD_REC_CANCEL,
    CMD_PLAY,
    CMD_PLAY_STOP,
    CMD_EXIT,
} vm_cmd_t;

typedef enum { WS_IDLE = 0, WS_RECORDING, WS_PAUSED, WS_PLAYING } work_state_t;

static TaskHandle_t s_worker;
static SemaphoreHandle_t s_idle;           // 一条命令完成后释放
static volatile work_state_t s_ws = WS_IDLE;
static FILE *s_rec_file;
static uint32_t s_rec_samples;
static uint32_t s_rec_ts;
static int s_play_idx = -1;
static char s_msg[64];
static volatile bool s_msg_valid;

static void notify_cmd(vm_cmd_t c) {
    if (s_worker) xTaskNotify(s_worker, (uint32_t)c, eSetValueWithOverwrite);
}

static void push_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_msg, sizeof(s_msg), fmt, ap);
    va_end(ap);
    s_msg_valid = true;
}

static void rec_update_ui(void) {
    if (!bsp_lvgl_lock(200)) return;
    if (s_rec_time) {
        uint32_t sec = s_rec_samples / SAMPLE_RATE;
        lv_label_set_text_fmt(s_rec_time, "%02u:%02u",
                              (unsigned)(sec / 60), (unsigned)(sec % 60));
    }
    bsp_lvgl_unlock();
}

static void wav_header_init(uint8_t *h, uint32_t rate) {
    uint32_t byte_rate = rate * 2;
    memset(h, 0, WAV_HEADER);
    memcpy(h, "RIFF", 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[20] = 1; h[22] = 1;
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byte_rate, 4);
    h[34] = 16; h[36] = 'd'; h[37] = 'a'; h[38] = 't'; h[39] = 'a';
}

static void wav_header_patch(FILE *f, uint32_t samples) {
    uint32_t data_size = samples * 2;
    uint32_t riff_size = data_size + 36;
    fseek(f, 4, SEEK_SET);
    fwrite(&riff_size, 4, 1, f);
    fseek(f, 40, SEEK_SET);
    fwrite(&data_size, 4, 1, f);
}

// 录音: 边录边写 SPIFFS; 暂停/继续/保存/放弃由命令驱动
static void do_record(void) {
    ensure_space();
    int64_t free_b = spiffs_free();
    if (free_b < (int64_t)(WAV_HEADER + BYTES_PER_SEC * 5)) {
        push_msg("存储已满,请删除旧录音");
        return;
    }
    uint32_t max_sec = (uint32_t)(free_b - 200u * 1024u) / BYTES_PER_SEC;
    if (max_sec > MAX_REC_SEC) max_sec = MAX_REC_SEC;
    if (max_sec < 5) max_sec = 5;

    s_rec_ts = now_epoch();
    char path[80];
    snprintf(path, sizeof(path), REC_PREFIX "%lu.wav", (unsigned long)s_rec_ts);
    FILE *f = fopen(path, "wb");
    if (!f) {
        push_msg("无法创建录音文件");
        return;
    }
    uint8_t hdr[WAV_HEADER];
    wav_header_init(hdr, SAMPLE_RATE);
    fwrite(hdr, 1, WAV_HEADER, f);

    if (bsp_audio_set_format(SAMPLE_RATE, 16, 1) != ESP_OK) {
        fclose(f);
        unlink(path);
        push_msg("音频初始化失败");
        return;
    }
    int16_t *buf = malloc(CHUNK_SAMPLES * sizeof(int16_t));
    if (!buf) {
        fclose(f);
        unlink(path);
        push_msg("内存不足");
        return;
    }

    s_rec_file = f;
    s_rec_samples = 0;
    s_ws = WS_RECORDING;
    push_msg("录音中");

    bool discard = false;
    uint32_t last_ui = 0;
    for (;;) {
        uint32_t n;
        if (xTaskNotifyWait(0, UINT32_MAX, &n, 0) == pdTRUE) {
            vm_cmd_t c = (vm_cmd_t)n;
            if (c == CMD_REC_PAUSE && s_ws == WS_RECORDING) { s_ws = WS_PAUSED; push_msg("已暂停"); }
            else if (c == CMD_REC_RESUME && s_ws == WS_PAUSED) { s_ws = WS_RECORDING; push_msg("继续记录"); }
            else if (c == CMD_REC_SAVE) break;
            else if (c == CMD_REC_CANCEL) { discard = true; break; }
        }
        if (s_ws == WS_PAUSED) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (s_rec_samples >= (uint32_t)max_sec * SAMPLE_RATE) {
            push_msg("达单条上限,自动保存");
            break;
        }
        if (bsp_audio_read(buf, CHUNK_SAMPLES * sizeof(int16_t)) != ESP_OK) {
            push_msg("麦克风读取失败");
            discard = true;
            break;
        }
        if (fwrite(buf, sizeof(int16_t), CHUNK_SAMPLES, f) != CHUNK_SAMPLES) {
            push_msg("写入失败,存储可能已满");
            discard = true;
            break;
        }
        s_rec_samples += CHUNK_SAMPLES;
        if (s_rec_samples - last_ui >= SAMPLE_RATE / 2) {
            last_ui = s_rec_samples;
            rec_update_ui();
        }
    }
    free(buf);
    s_rec_file = NULL;
    bool keep = !discard && s_rec_samples >= SAMPLE_RATE / 2;   // 至少 0.5s
    fflush(f);
    if (keep) wav_header_patch(f, s_rec_samples);
    fclose(f);
    if (!keep) {
        snprintf(path, sizeof(path), REC_PREFIX "%lu.wav", (unsigned long)s_rec_ts);
        unlink(path);
        if (!discard) push_msg("太短,已丢弃");
        else push_msg("已放弃");
    } else {
        push_msg("已保存");
    }
    s_ws = WS_IDLE;
}

static void do_play(int idx) {
    if (idx < 0 || idx >= s_rec_cnt) return;
    char path[80];
    snprintf(path, sizeof(path), MOUNT_BASE "/%s", s_recs[idx].name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        push_msg("文件不存在");
        return;
    }
    fseek(f, WAV_HEADER, SEEK_SET);
    if (bsp_audio_set_format(SAMPLE_RATE, 16, 1) != ESP_OK) {
        fclose(f);
        push_msg("音频初始化失败");
        return;
    }
    bsp_audio_set_volume(85);
    int16_t *buf = malloc(CHUNK_SAMPLES * sizeof(int16_t));
    if (!buf) {
        fclose(f);
        push_msg("内存不足");
        return;
    }
    s_play_idx = idx;
    s_ws = WS_PLAYING;
    push_msg("播放中");
    for (;;) {
        uint32_t n;
        if (xTaskNotifyWait(0, UINT32_MAX, &n, 0) == pdTRUE) {
            if ((vm_cmd_t)n == CMD_PLAY_STOP) break;
        }
        size_t got = fread(buf, sizeof(int16_t), CHUNK_SAMPLES, f);
        if (got == 0) break;
        if (bsp_audio_write(buf, got * sizeof(int16_t)) != ESP_OK) {
            push_msg("播放失败");
            break;
        }
    }
    free(buf);
    fclose(f);
    s_play_idx = -1;
    s_ws = WS_IDLE;
}

static void worker_task(void *arg) {
    (void)arg;
    bool running = true;
    while (running) {
        uint32_t n = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &n, portMAX_DELAY) != pdTRUE) continue;
        vm_cmd_t c = (vm_cmd_t)n;
        switch (c) {
        case CMD_EXIT:
            running = false;
            break;
        case CMD_REC_START:
            do_record();
            xSemaphoreGive(s_idle);
            break;
        case CMD_PLAY:
            do_play(s_play_idx >= 0 ? s_play_idx : s_sel - 1);
            xSemaphoreGive(s_idle);
            break;
        default:
            break;
        }
    }
    s_worker = NULL;
    xSemaphoreGive(s_idle);
    vTaskDelete(NULL);
}

static bool worker_start(void) {
    if (s_worker) return true;
    if (s_idle) { vSemaphoreDelete(s_idle); s_idle = NULL; }
    s_idle = xSemaphoreCreateBinary();
    if (!s_idle) return false;
    if (xTaskCreate(worker_task, "vm_work", 8192, NULL, 4, &s_worker) != pdPASS) {
        vSemaphoreDelete(s_idle);
        s_idle = NULL;
        return false;
    }
    return true;
}

static void worker_stop(void) {
    TaskHandle_t t = s_worker;
    if (!t) return;
    if (s_ws == WS_RECORDING || s_ws == WS_PAUSED) {
        notify_cmd(CMD_REC_SAVE);                 // 退出页面时保存而不是丢弃
    } else if (s_ws == WS_PLAYING) {
        notify_cmd(CMD_PLAY_STOP);
    }
    notify_cmd(CMD_EXIT);
    if (xSemaphoreTake(s_idle, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "worker stop 超时");
    }
    s_worker = NULL;
    if (s_idle) { vSemaphoreDelete(s_idle); s_idle = NULL; }
    s_ws = WS_IDLE;
}

// ---------- 各页渲染 ----------
static void fmt_ts(uint32_t e, char *out, size_t len) {
    if (!s_time_set) {
        snprintf(out, len, "未设时间");
        return;
    }
    int y, mo, d, hh, mi;
    epoch_to_local(e, &y, &mo, &d, &hh, &mi);
    snprintf(out, len, "%02d-%02d %02d:%02d", mo, d, hh, mi);
}

// 时间设置字段(文件域, 便于按键处理)
static int s_tf[5];      // 年 月 日 时 分

static void render(void) {
    if (!s_rows_box) return;
    lv_obj_clean(s_rows_box);
    s_rec_time = NULL;
    char line[96];
    char tbuf[24];

    if (s_page == PG_HOME) {
        const char *rows[] = { "● 碎碎念", "+ 待办记录", "t 时间设置" };
        for (int i = 0; i < 3; i++) {
            snprintf(line, sizeof(line), "%s%s", s_sel == i ? "> " : "  ", rows[i]);
            add_row(i, line, s_sel == i);
        }
    } else if (s_page == PG_SUI) {
        clamp_sel(s_rec_cnt);
        snprintf(line, sizeof(line), "%s● 开始录音", s_sel == 0 ? "> " : "  ");
        add_row(0, line, s_sel == 0);
        int shown = 0;
        for (int i = s_scroll; i < s_rec_cnt && shown < VIS_ROWS - 1; i++, shown++) {
            fmt_ts(s_recs[i].ts, tbuf, sizeof(tbuf));
            snprintf(line, sizeof(line), "%s%u:%02u %s%s",
                     s_sel == i + 1 ? "> " : "  ",
                     (unsigned)(s_recs[i].dur / 60), (unsigned)(s_recs[i].dur % 60),
                     tbuf, s_play_idx == i ? " ●" : "");
            add_row(shown + 1, line, s_sel == i + 1);
        }
        if (s_rec_cnt == 0) {
            mk_label(s_rows_box, "空空如也,录一条吧", 36, 140, 180, UI_INK);
        }
    } else if (s_page == PG_REC) {
        s_rec_time = mk_label(s_rows_box, "00:00", 62, 40, 100, UI_INK);
        lv_obj_set_style_text_font(s_rec_time, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_align(s_rec_time, LV_TEXT_ALIGN_CENTER, 0);
        const char *hint = (s_ws == WS_PAUSED)
            ? "OK短按=保存  上键长按=继续\n下键长按=放弃"
            : "OK短按=暂停  下键长按=放弃";
        mk_label(s_rows_box, hint, 8, 120, 208, UI_INK);
    } else if (s_page == PG_TODO) {
        clamp_sel(s_todo_cnt);
        snprintf(line, sizeof(line), "%s+ 新增待办", s_sel == 0 ? "> " : "  ");
        add_row(0, line, s_sel == 0);
        int shown = 0;
        for (int i = s_scroll; i < s_todo_cnt && shown < VIS_ROWS - 1; i++, shown++) {
            fmt_ts(s_todos[i].ts, tbuf, sizeof(tbuf));
            snprintf(line, sizeof(line), "%s[%c]%s %s",
                     s_sel == i + 1 ? "> " : "  ",
                     s_todos[i].done ? 'v' : ' ',
                     s_todos[i].text, tbuf);
            add_row(shown + 1, line, s_sel == i + 1);
        }
    } else if (s_page == PG_TPL) {
        for (int i = 0; i < (int)TPL_COUNT + 1; i++) {
            if (i == 0) snprintf(line, sizeof(line), "%sx 取消", s_sel == 0 ? "> " : "  ");
            else snprintf(line, sizeof(line), "%s%s", s_sel == i ? "> " : "  ", TEMPLATES[i - 1]);
            add_row(i, line, s_sel == i);
        }
    } else if (s_page == PG_TIME) {
        static const char *const names[5] = { "年", "月", "日", "时", "分" };
        snprintf(line, sizeof(line), "%sx 取消", s_sel == 0 ? "> " : "  ");
        add_row(0, line, s_sel == 0);
        for (int i = 0; i < 5; i++) {
            snprintf(line, sizeof(line), "%s%s: %d", s_sel == i + 1 ? "> " : "  ",
                     names[i], s_tf[i]);
            add_row(i + 1, line, s_sel == i + 1);
        }
        snprintf(line, sizeof(line), "%sOK 保存时间", s_sel == 6 ? "> " : "  ");
        add_row(6, line, s_sel == 6);
    }

    if (s_footer) {
        int64_t fr = spiffs_free();
        if (s_time_set) {
            char tb[24];
            fmt_ts(now_epoch(), tb, sizeof(tb));
            lv_label_set_text_fmt(s_footer, "%s 余%uKB", tb, (unsigned)(fr < 0 ? 0 : fr / 1024));
        } else {
            lv_label_set_text_fmt(s_footer, "未设时间 余%uKB", (unsigned)(fr < 0 ? 0 : fr / 1024));
        }
    }
}

static void flash_msg(void) {
    if (s_msg_valid && s_footer && bsp_lvgl_lock(300)) {
        lv_label_set_text(s_footer, s_msg);
        bsp_lvgl_unlock();
    }
    s_msg_valid = false;
}

// ---------- 页面切换 ----------
static void goto_page(page_t p) {
    s_page = p;
    s_sel = 0;
    s_scroll = 0;
    s_rec_time = NULL;
    if (p == PG_TIME) {
        if (s_time_set) {
            int y, mo, d, hh, mi;
            epoch_to_local(now_epoch(), &y, &mo, &d, &hh, &mi);
            s_tf[0] = y; s_tf[1] = mo; s_tf[2] = d; s_tf[3] = hh; s_tf[4] = mi;
        } else {
            s_tf[0] = 2026; s_tf[1] = 1; s_tf[2] = 1; s_tf[3] = 8; s_tf[4] = 0;
        }
    }
    render();
}

// ---------- 时间字段编辑 ----------
static void time_mutate(int row, int delta) {
    int i = row - 1;
    if (i == 0)      { s_tf[0] += delta; if (s_tf[0] < 2020) s_tf[0] = 2035; if (s_tf[0] > 2035) s_tf[0] = 2020; }
    else if (i == 1) { s_tf[1] += delta; if (s_tf[1] < 1) s_tf[1] = 12; if (s_tf[1] > 12) s_tf[1] = 1; }
    else if (i == 2) { s_tf[2] += delta; if (s_tf[2] < 1) s_tf[2] = 31; if (s_tf[2] > 31) s_tf[2] = 1; }
    else if (i == 3) { s_tf[3] += delta; if (s_tf[3] < 0) s_tf[3] = 23; if (s_tf[3] > 23) s_tf[3] = 0; }
    else if (i == 4) { s_tf[4] += delta; if (s_tf[4] < 0) s_tf[4] = 59; if (s_tf[4] > 59) s_tf[4] = 0; }
}

// ---------- 生命周期(demo_entry_t) ----------
void voice_memo_enter(void) {
    build_page_shell("碎碎念护照");
    goto_page(PG_HOME);
    lv_screen_load(s_scr);
}

void voice_memo_exit(void) {
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_rows_box = NULL;
        s_footer = NULL;
        s_rec_time = NULL;
    }
}

esp_err_t voice_memo_start(void) {
    // SPIFFS: 挂载失败(首次/损坏)则格式化
    esp_vfs_spiffs_conf_t conf = {
        .base_path = MOUNT_BASE,
        .partition_label = "store",
        .max_files = 3,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS 挂载失败: %s", esp_err_to_name(ret));
        return ret;
    }
    // NVS: 存时间基准
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }
    time_load();
    todos_load();
    if (!worker_start()) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

esp_err_t voice_memo_stop(void) {
    worker_stop();
    return ESP_OK;
}

// ---------- 按键 ----------
void voice_memo_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    const bool long_ev = (ev == BSP_BTN_LONG);
    const bool click = (ev == BSP_BTN_CLICK);
    if (!click && !long_ev) return;

    // 上键长按: 仅录音暂停态 = 继续记录
    if (long_ev && btn == BSP_BTN_UP) {
        if (s_page == PG_REC && s_ws == WS_PAUSED) {
            notify_cmd(CMD_REC_RESUME);
        }
        return;
    }

    // 下键长按: 删除 / 放弃 / 时间字段减一
    if (long_ev && btn == BSP_BTN_DOWN) {
        if (s_page == PG_REC) {
            if (s_ws == WS_RECORDING || s_ws == WS_PAUSED) notify_cmd(CMD_REC_CANCEL);
        } else if (s_page == PG_SUI && s_sel >= 1 && s_sel <= s_rec_cnt) {
            if (s_ws == WS_PLAYING && s_play_idx == s_sel - 1) notify_cmd(CMD_PLAY_STOP);
            delete_rec(s_sel - 1);
            if (s_sel > s_rec_cnt) s_sel = s_rec_cnt;
            push_msg("已删除");
            if (bsp_lvgl_lock(300)) { render(); flash_msg(); bsp_lvgl_unlock(); }
        } else if (s_page == PG_TODO && s_sel >= 1 && s_sel <= s_todo_cnt) {
            memmove(&s_todos[s_sel - 1], &s_todos[s_sel],
                    sizeof(todo_t) * (size_t)(s_todo_cnt - s_sel));
            s_todo_cnt--;
            todos_save();
            if (s_sel > s_todo_cnt) s_sel = s_todo_cnt;
            push_msg("已删除");
            if (bsp_lvgl_lock(300)) { render(); flash_msg(); bsp_lvgl_unlock(); }
        } else if (s_page == PG_TIME && s_sel >= 1 && s_sel <= 5) {
            time_mutate(s_sel, -1);
            if (bsp_lvgl_lock(300)) { render(); bsp_lvgl_unlock(); }
        }
        return;
    }

    if (!click) return;

    // 上/下短按: 移动选中
    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        int len = 1;
        if (s_page == PG_HOME) len = 3;
        else if (s_page == PG_SUI) len = s_rec_cnt + 1;
        else if (s_page == PG_TODO) len = s_todo_cnt + 1;
        else if (s_page == PG_TPL) len = (int)TPL_COUNT + 1;
        else if (s_page == PG_TIME) len = 7;
        if (len <= 0) return;
        s_sel = (s_sel + (btn == BSP_BTN_DOWN ? 1 : -1) + len) % len;
        if (bsp_lvgl_lock(300)) { render(); bsp_lvgl_unlock(); }
        return;
    }

    // OK 短按
    if (btn != BSP_BTN_OK) return;

    if (s_page == PG_HOME) {
        if (s_sel == 0) goto_page(PG_SUI);
        else if (s_sel == 1) goto_page(PG_TODO);
        else goto_page(PG_TIME);
    } else if (s_page == PG_SUI) {
        if (s_sel == 0) {
            if (s_ws == WS_IDLE) {
                s_play_idx = -1;
                notify_cmd(CMD_REC_START);
                goto_page(PG_REC);
            }
        } else {
            int idx = s_sel - 1;
            if (s_ws == WS_PLAYING && s_play_idx == idx) {
                notify_cmd(CMD_PLAY_STOP);
            } else if (s_ws == WS_IDLE) {
                s_play_idx = idx;
                notify_cmd(CMD_PLAY);
            }
        }
    } else if (s_page == PG_REC) {
        if (s_ws == WS_RECORDING) {
            notify_cmd(CMD_REC_PAUSE);
        } else if (s_ws == WS_PAUSED) {
            notify_cmd(CMD_REC_SAVE);
            xSemaphoreTake(s_idle, pdMS_TO_TICKS(3000));   // 等待落盘
            scan_recs();
            goto_page(PG_SUI);
            s_sel = 1;
            render();
        }
    } else if (s_page == PG_TODO) {
        if (s_sel == 0) {
            goto_page(PG_TPL);
        } else {
            s_todos[s_sel - 1].done = !s_todos[s_sel - 1].done;
            todos_save();
            if (bsp_lvgl_lock(300)) { render(); bsp_lvgl_unlock(); }
        }
    } else if (s_page == PG_TPL) {
        if (s_sel == 0) {
            goto_page(PG_TODO);
        } else {
            todo_add(TEMPLATES[s_sel - 1]);
            push_msg("已添加");
            goto_page(PG_TODO);
            flash_msg();
        }
    } else if (s_page == PG_TIME) {
        if (s_sel == 0) {
            goto_page(PG_HOME);
        } else if (s_sel == 6) {
            time_store(civil_to_epoch(s_tf[0], s_tf[1], s_tf[2], s_tf[3], s_tf[4]));
            push_msg("时间已保存");
            goto_page(PG_HOME);
            flash_msg();
        } else {
            time_mutate(s_sel, 1);
            if (bsp_lvgl_lock(300)) { render(); bsp_lvgl_unlock(); }
        }
    }

    flash_msg();
}
