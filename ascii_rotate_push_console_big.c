
/*
 * ascii_rotate_push_console_win.c
 *
 * Windows 命令行综合版：
 *   左键拖动：旋转 ASCII 肖像
 *   右键拖动：把 ASCII 字符推开，松手后回弹
 *
 * 不开 GUI 窗口，不用 SDL，只用 Windows Console API。
 *
 * 编译：
 *   gcc ascii_rotate_push_console_big.c -o ascii_rotate_push_console_big.exe -lm
 *
 * 运行：
 *   .\ascii_rotate_push_console_big.exe .\pure_ascii_face_finger_160col.txt 35 1.00 2.5
 *   .\ascii_rotate_push_console_big.exe .\pure_ascii_face_finger_220col.txt 35 0.72 2.5
 *
 * 参数：
 *   argv[1] = ASCII txt 文件路径
 *   argv[2] = 每帧延迟 ms，默认 35。越小越快。
 *   argv[3] = 缩放系数，默认 1.00。220 列建议 0.70 ~ 0.80。
 *   argv[4] = 右键推开幅度，默认 2.5。越大越夸张。
 *
 * 操作：
 *   鼠标左键拖动：旋转
 *   鼠标右键拖动：推开/扰动
 *   R：复位角度和扰动
 *   Space：开关自动慢速旋转
 *   Q 或 Esc：退出
 *
 * 注意：
 *   1. 建议先用 160col 文件。
 *   2. 终端窗口要尽量拉大，否则画面会被裁剪。
 *   3. 如果鼠标没反应，优先用 Windows Terminal 或 cmd，并关闭 QuickEdit Mode。
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef MOUSE_MOVED
#define MOUSE_MOVED 0x0001
#endif

#define MAX_LINES 1200
#define MAX_WIDTH 2400

typedef struct {
    float dx, dy;     /* 屏幕空间位移，单位是控制台字符格 */
    float vx, vy;
    float sx, sy;     /* 当前旋转投影后的屏幕坐标 */
    float depth;      /* 当前深度 */
} Node;

static char g_lines[MAX_LINES][MAX_WIDTH];
static char *g_grid = NULL;
static Node *g_nodes = NULL;
static Node *g_tmp = NULL;

static CHAR_INFO *g_screen = NULL;
static float *g_zbuf = NULL;

static int g_rows = 0;
static int g_cols = 0;
static int g_total = 0;

static int g_out_w = 0;
static int g_out_h = 0;

static HANDLE g_hout = NULL;
static HANDLE g_hin = NULL;
static WORD g_attr = 0;

static int g_running = 1;
static int g_auto_spin = 0;
static float g_push_amp = 2.5f;  /* 右键推开幅度，默认 2.5 */

static int g_left_down = 0;
static int g_right_down = 0;
static int g_last_mx = 0;
static int g_last_my = 0;

static double g_yaw = 0.0;
static double g_pitch = 0.0;
static double g_roll = 0.0;

static float g_right_dx_acc = 0.0f;
static float g_right_dy_acc = 0.0f;
static int g_mouse_x = 0;
static int g_mouse_y = 0;

static int read_ascii_file(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "无法打开文件: %s\n", path);
        return 0;
    }

    char buf[MAX_WIDTH];
    int rows = 0;
    int cols = 0;

    while (fgets(buf, sizeof(buf), fp) && rows < MAX_LINES) {
        size_t len = strlen(buf);

        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
            buf[--len] = '\0';
        }

        if ((int)len > cols) cols = (int)len;

        strncpy(g_lines[rows], buf, MAX_WIDTH - 1);
        g_lines[rows][MAX_WIDTH - 1] = '\0';
        rows++;
    }

    fclose(fp);

    if (rows <= 0 || cols <= 0) {
        fprintf(stderr, "文件为空。\n");
        return 0;
    }

    g_rows = rows;
    g_cols = cols;
    g_total = g_rows * g_cols;

    g_grid = (char *)malloc((size_t)g_total);
    g_nodes = (Node *)calloc((size_t)g_total, sizeof(Node));
    g_tmp = (Node *)calloc((size_t)g_total, sizeof(Node));

    if (!g_grid || !g_nodes || !g_tmp) {
        fprintf(stderr, "内存分配失败。\n");
        return 0;
    }

    for (int y = 0; y < g_rows; y++) {
        int len = (int)strlen(g_lines[y]);
        for (int x = 0; x < g_cols; x++) {
            char c = (x < len) ? g_lines[y][x] : ' ';
            if ((unsigned char)c > 127) c = '?';
            g_grid[y * g_cols + x] = c;
        }
    }

    return 1;
}

static int char_density_index(char c) {
    const char *chars = "@%#*+=-:. ";
    const char *p = strchr(chars, c);
    if (!p) return 4;
    return (int)(p - chars);
}

static float char_relief_z(char c) {
    int idx = char_density_index(c);
    int max_idx = 9;

    float density = 1.0f - (float)idx / (float)max_idx;
    return density * 16.0f - 8.0f;
}

static void reset_all(void) {
    g_yaw = 0.0;
    g_pitch = 0.0;
    g_roll = 0.0;

    if (!g_nodes) return;

    for (int i = 0; i < g_total; i++) {
        g_nodes[i].dx = 0.0f;
        g_nodes[i].dy = 0.0f;
        g_nodes[i].vx = 0.0f;
        g_nodes[i].vy = 0.0f;
    }
}

static void setup_console(void) {
    g_hout = GetStdHandle(STD_OUTPUT_HANDLE);
    g_hin = GetStdHandle(STD_INPUT_HANDLE);

    /*
     * 亮白背景 + 黑色前景。
     */
    g_attr = BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE | BACKGROUND_INTENSITY;

    DWORD in_mode = 0;
    GetConsoleMode(g_hin, &in_mode);

    /*
     * 关闭 QuickEdit，否则鼠标拖动会进入文本选择，程序收不到鼠标拖动事件。
     */
    in_mode |= ENABLE_EXTENDED_FLAGS;
    in_mode &= ~ENABLE_QUICK_EDIT_MODE;
    in_mode |= ENABLE_MOUSE_INPUT;
    in_mode |= ENABLE_WINDOW_INPUT;
    SetConsoleMode(g_hin, in_mode);

    CONSOLE_CURSOR_INFO ci;
    GetConsoleCursorInfo(g_hout, &ci);
    ci.bVisible = FALSE;
    SetConsoleCursorInfo(g_hout, &ci);

    COORD size;
    size.X = (SHORT)g_out_w;
    size.Y = (SHORT)g_out_h;
    SetConsoleScreenBufferSize(g_hout, size);

    SMALL_RECT rect;
    rect.Left = 0;
    rect.Top = 0;
    rect.Right = (SHORT)(g_out_w - 1);
    rect.Bottom = (SHORT)(g_out_h - 1);
    SetConsoleWindowInfo(g_hout, TRUE, &rect);
}

static void restore_console(void) {
    if (!g_hout) return;

    CONSOLE_CURSOR_INFO ci;
    GetConsoleCursorInfo(g_hout, &ci);
    ci.bVisible = TRUE;
    SetConsoleCursorInfo(g_hout, &ci);

    SetConsoleTextAttribute(
        g_hout,
        FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE
    );
}

static void clear_buffers(void) {
    int total = g_out_w * g_out_h;

    for (int i = 0; i < total; i++) {
        g_screen[i].Char.AsciiChar = ' ';
        g_screen[i].Attributes = g_attr;
        g_zbuf[i] = -1.0e30f;
    }
}

static void handle_input(void) {
    DWORD count = 0;
    GetNumberOfConsoleInputEvents(g_hin, &count);

    while (count > 0) {
        INPUT_RECORD rec;
        DWORD read_count = 0;

        if (!ReadConsoleInputA(g_hin, &rec, 1, &read_count)) {
            break;
        }

        if (rec.EventType == KEY_EVENT) {
            KEY_EVENT_RECORD k = rec.Event.KeyEvent;

            if (k.bKeyDown) {
                WORD vk = k.wVirtualKeyCode;
                CHAR ch = k.uChar.AsciiChar;

                if (vk == VK_ESCAPE || ch == 'q' || ch == 'Q') {
                    g_running = 0;
                } else if (vk == VK_SPACE) {
                    g_auto_spin = !g_auto_spin;
                } else if (ch == 'r' || ch == 'R') {
                    reset_all();
                }
            }
        } else if (rec.EventType == MOUSE_EVENT) {
            MOUSE_EVENT_RECORD m = rec.Event.MouseEvent;

            int mx = m.dwMousePosition.X;
            int my = m.dwMousePosition.Y;

            int old_left = g_left_down;
            int old_right = g_right_down;

            g_left_down = (m.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
            g_right_down = (m.dwButtonState & RIGHTMOST_BUTTON_PRESSED) != 0;

            if (!old_left && !old_right && (g_left_down || g_right_down)) {
                g_last_mx = mx;
                g_last_my = my;
            }

            int mdx = mx - g_last_mx;
            int mdy = my - g_last_my;

            if (g_left_down) {
                /*
                 * 左键：旋转。
                 */
                g_yaw += (double)mdx * 0.045;
                g_pitch += (double)mdy * 0.035;

                if (g_pitch > 1.25) g_pitch = 1.25;
                if (g_pitch < -1.25) g_pitch = -1.25;
            }

            if (g_right_down) {
                /*
                 * 右键：屏幕空间推开。
                 */
                g_right_dx_acc += (float)mdx;
                g_right_dy_acc += (float)mdy;
                g_mouse_x = mx;
                g_mouse_y = my;
            }

            g_last_mx = mx;
            g_last_my = my;
        }

        GetNumberOfConsoleInputEvents(g_hin, &count);
    }
}

static void project_nodes(double zoom) {
    double center_x = (double)(g_cols - 1) * 0.5;
    double center_y = (double)(g_rows - 1) * 0.5;

    double cosY = cos(g_yaw);
    double sinY = sin(g_yaw);

    double cosX = cos(g_pitch);
    double sinX = sin(g_pitch);

    double cosZ = cos(g_roll);
    double sinZ = sin(g_roll);

    /*
     * 透视距离。越小透视越强。
     */
    double camera = 230.0;

    for (int y = 0; y < g_rows; y++) {
        for (int x = 0; x < g_cols; x++) {
            int id = y * g_cols + x;
            char c = g_grid[id];

            if (c == ' ') {
                g_nodes[id].sx = -99999.0f;
                g_nodes[id].sy = -99999.0f;
                g_nodes[id].depth = -99999.0f;
                continue;
            }

            double px = ((double)x - center_x) * zoom;
            double py = ((double)y - center_y) * zoom;
            double pz = (double)char_relief_z(c) * zoom;

            /*
             * 轻微绕 Z 轴，避免完全像纸片。
             */
            double xz = px * cosZ - py * sinZ;
            double yz = px * sinZ + py * cosZ;
            double zz = pz;

            /*
             * 绕 Y 轴左右旋转。
             */
            double x1 = xz * cosY + zz * sinY;
            double z1 = -xz * sinY + zz * cosY;
            double y1 = yz;

            /*
             * 绕 X 轴上下倾斜。
             */
            double y2 = y1 * cosX - z1 * sinX;
            double z2 = y1 * sinX + z1 * cosX;
            double x2 = x1;

            double denom = camera + z2;
            if (denom < 20.0) {
                g_nodes[id].sx = -99999.0f;
                g_nodes[id].sy = -99999.0f;
                g_nodes[id].depth = -99999.0f;
                continue;
            }

            double k = camera / denom;

            g_nodes[id].sx = (float)((double)g_out_w * 0.5 + x2 * k);
            g_nodes[id].sy = (float)((double)g_out_h * 0.5 + y2 * k);
            g_nodes[id].depth = (float)(-z2);
        }
    }
}

static void apply_right_mouse_force(void) {
    if (!g_right_down) {
        g_right_dx_acc = 0.0f;
        g_right_dy_acc = 0.0f;
        return;
    }

    const float radius = 22.0f * g_push_amp;
    const float radius2 = radius * radius;

    const float drag_gain = 1.25f * g_push_amp;
    const float push_gain = 0.90f * g_push_amp;

    for (int i = 0; i < g_total; i++) {
        if (g_grid[i] == ' ') continue;

        float sx = g_nodes[i].sx + g_nodes[i].dx;
        float sy = g_nodes[i].sy + g_nodes[i].dy;

        float rx = sx - (float)g_mouse_x;
        float ry = sy - (float)g_mouse_y;
        float r2 = rx * rx + ry * ry;

        if (r2 < radius2) {
            float t = 1.0f - r2 / radius2;
            float w = t * t;
            float dist = sqrtf(r2) + 0.001f;

            float nx = rx / dist;
            float ny = ry / dist;

            g_nodes[i].vx += (g_right_dx_acc * drag_gain + nx * push_gain) * w;
            g_nodes[i].vy += (g_right_dy_acc * drag_gain + ny * push_gain) * w;
        }
    }

    /*
     * 鼠标增量只用一帧，否则会持续加速。
     */
    g_right_dx_acc = 0.0f;
    g_right_dy_acc = 0.0f;
}

static void update_physics(float dt) {
    const float spring = 22.0f;
    const float coupling = 42.0f;
    const float damping = 3.2f;

    float damp = expf(-damping * dt);

    for (int y = 0; y < g_rows; y++) {
        for (int x = 0; x < g_cols; x++) {
            int id = y * g_cols + x;

            if (g_grid[id] == ' ') {
                g_tmp[id] = g_nodes[id];
                continue;
            }

            float avg_dx = 0.0f;
            float avg_dy = 0.0f;
            int cnt = 0;

            if (x > 0) {
                int j = y * g_cols + x - 1;
                if (g_grid[j] != ' ') {
                    avg_dx += g_nodes[j].dx;
                    avg_dy += g_nodes[j].dy;
                    cnt++;
                }
            }
            if (x + 1 < g_cols) {
                int j = y * g_cols + x + 1;
                if (g_grid[j] != ' ') {
                    avg_dx += g_nodes[j].dx;
                    avg_dy += g_nodes[j].dy;
                    cnt++;
                }
            }
            if (y > 0) {
                int j = (y - 1) * g_cols + x;
                if (g_grid[j] != ' ') {
                    avg_dx += g_nodes[j].dx;
                    avg_dy += g_nodes[j].dy;
                    cnt++;
                }
            }
            if (y + 1 < g_rows) {
                int j = (y + 1) * g_cols + x;
                if (g_grid[j] != ' ') {
                    avg_dx += g_nodes[j].dx;
                    avg_dy += g_nodes[j].dy;
                    cnt++;
                }
            }

            if (cnt > 0) {
                avg_dx /= (float)cnt;
                avg_dy /= (float)cnt;
            }

            float ax = -spring * g_nodes[id].dx + coupling * (avg_dx - g_nodes[id].dx);
            float ay = -spring * g_nodes[id].dy + coupling * (avg_dy - g_nodes[id].dy);

            g_tmp[id] = g_nodes[id];

            g_tmp[id].vx += ax * dt;
            g_tmp[id].vy += ay * dt;

            g_tmp[id].vx *= damp;
            g_tmp[id].vy *= damp;

            g_tmp[id].dx += g_tmp[id].vx * dt;
            g_tmp[id].dy += g_tmp[id].vy * dt;

            if (g_tmp[id].dx > 45.0f) g_tmp[id].dx = 45.0f;
            if (g_tmp[id].dx < -45.0f) g_tmp[id].dx = -45.0f;
            if (g_tmp[id].dy > 45.0f) g_tmp[id].dy = 45.0f;
            if (g_tmp[id].dy < -45.0f) g_tmp[id].dy = -45.0f;
        }
    }

    memcpy(g_nodes, g_tmp, sizeof(Node) * (size_t)g_total);
}

static void plot_point(int sx, int sy, float depth, char c) {
    if (sx < 0 || sx >= g_out_w || sy < 0 || sy >= g_out_h) {
        return;
    }

    int id = sy * g_out_w + sx;

    if (depth > g_zbuf[id]) {
        g_zbuf[id] = depth;
        g_screen[id].Char.AsciiChar = c;
        g_screen[id].Attributes = g_attr;
    }
}

static void render_frame(double zoom) {
    clear_buffers();
    project_nodes(zoom);

    for (int i = 0; i < g_total; i++) {
        char c = g_grid[i];

        if (c == ' ') continue;

        int sx = (int)roundf(g_nodes[i].sx + g_nodes[i].dx);
        int sy = (int)roundf(g_nodes[i].sy + g_nodes[i].dy);

        plot_point(sx, sy, g_nodes[i].depth, c);
    }

    COORD buffer_size;
    buffer_size.X = (SHORT)g_out_w;
    buffer_size.Y = (SHORT)g_out_h;

    COORD buffer_coord;
    buffer_coord.X = 0;
    buffer_coord.Y = 0;

    SMALL_RECT write_region;
    write_region.Left = 0;
    write_region.Top = 0;
    write_region.Right = (SHORT)(g_out_w - 1);
    write_region.Bottom = (SHORT)(g_out_h - 1);

    WriteConsoleOutputA(
        g_hout,
        g_screen,
        buffer_size,
        buffer_coord,
        &write_region
    );
}

int main(int argc, char **argv) {
    const char *path = "pure_ascii_face_finger_160col.txt";
    int frame_ms = 35;
    double zoom = 1.0;

    if (argc >= 2) {
        path = argv[1];
    }

    if (argc >= 3) {
        frame_ms = atoi(argv[2]);
        if (frame_ms < 5) frame_ms = 5;
        if (frame_ms > 300) frame_ms = 300;
    }

    if (argc >= 4) {
        zoom = atof(argv[3]);
        if (zoom < 0.25) zoom = 0.25;
        if (zoom > 2.50) zoom = 2.50;
    }

    /*
     * argv[4] = right-button push amplitude.
     * 1.0 = mild, 2.5 = strong, 4.0 = very strong.
     */
    if (argc >= 5) {
        g_push_amp = (float)atof(argv[4]);
        if (g_push_amp < 0.5f) g_push_amp = 0.5f;
        if (g_push_amp > 6.0f) g_push_amp = 6.0f;
    }

    if (!read_ascii_file(path)) {
        return 1;
    }

    g_out_w = (int)(g_cols * zoom + 28);
    g_out_h = (int)(g_rows * zoom + 14);

    if (g_out_w < 80) g_out_w = 80;
    if (g_out_h < 30) g_out_h = 30;

    /*
     * 控制台窗口太大时容易设置失败；这里做上限。
     * 220 列文件建议 zoom 0.70 ~ 0.80。
     */
    if (g_out_w > 220) g_out_w = 220;
    if (g_out_h > 90) g_out_h = 90;

    int out_total = g_out_w * g_out_h;

    g_screen = (CHAR_INFO *)malloc(sizeof(CHAR_INFO) * (size_t)out_total);
    g_zbuf = (float *)malloc(sizeof(float) * (size_t)out_total);

    if (!g_screen || !g_zbuf) {
        fprintf(stderr, "内存分配失败。\n");
        free(g_grid);
        free(g_nodes);
        free(g_tmp);
        return 1;
    }

    setup_console();

    float dt = (float)frame_ms / 1000.0f;

    while (g_running) {
        handle_input();

        if (g_auto_spin) {
            g_yaw += 0.035;
            g_roll = 0.08 * sin(g_yaw * 0.6);
        }

        project_nodes(zoom);
        apply_right_mouse_force();
        update_physics(dt);
        render_frame(zoom);

        Sleep((DWORD)frame_ms);
    }

    restore_console();

    free(g_grid);
    free(g_nodes);
    free(g_tmp);
    free(g_screen);
    free(g_zbuf);

    return 0;
}
