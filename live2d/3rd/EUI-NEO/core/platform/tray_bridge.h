#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int eui_tray_init(const char* icon_path);
int eui_tray_is_initialized(void);
void eui_tray_poll(int blocking);
int eui_tray_consume_show_requested(void);
int eui_tray_consume_exit_requested(void);
void eui_tray_shutdown(void);

/* l2dpet 补丁：应用自定义托盘菜单（平铺数组，parent = 父项在数组里的下标，-1 = 顶层）。
 * count = 0 恢复默认的 Show / Exit。点击结果用 eui_tray_consume_menu_click 取（-1 = 没有）。 */
typedef struct eui_tray_item {
    int id;
    const char* label;
    int checked;
    int enabled;
    int separator;
    int radio;
    int parent;
    int checkable;   /* 勾选项（没勾上也显示成可勾选） */
} eui_tray_item;

void eui_tray_set_menu(const eui_tray_item* items, int count);
int eui_tray_consume_menu_click(void);
void eui_tray_set_title(const char* title);

#ifdef __cplusplus
}
#endif
