#!/bin/sh
# .desktop 先解析 key/value 转义，再解析 Exec 引号与字段码；两层分别处理。
desktop_entry_exec() {
    L2DPET_ENTRY_PATH=$1 awk 'BEGIN {
        s = ENVIRON["L2DPET_ENTRY_PATH"]; out = "\""
        for (i = 1; i <= length(s); i++) {
            c = substr(s, i, 1)
            if (c == "\\") out = out "\\\\\\\\"
            else if (c == "\"" || c == "$" || c == "`") out = out "\\\\" c
            else if (c == "%") out = out "%%"
            else if (c == "\n") out = out "\\n"
            else if (c == "\t") out = out "\\t"
            else out = out c
        }
        # GLib 在展开 %% 之前检查第一个可执行文件；用固定 env 入口支持路径中的 %。
        print "/usr/bin/env " out "\""
    }'
}

write_desktop_file() {  # target, executable (one path, no arguments), icon
    entry_template=$(desktop_template) || return 1
    entry_exec=$(desktop_entry_exec "$2") || return 1
    L2DPET_ENTRY_EXEC=$entry_exec L2DPET_ENTRY_ICON=$3 awk '
        BEGIN {
            s = ENVIRON["L2DPET_ENTRY_ICON"]; icon = ""
            for (i = 1; i <= length(s); i++) {
                c = substr(s, i, 1)
                if (c == "\\") icon = icon "\\\\"
                else if (c == "\n") icon = icon "\\n"
                else if (c == "\t") icon = icon "\\t"
                else icon = icon c
            }
        }
        /^Exec=/ { print "Exec=" ENVIRON["L2DPET_ENTRY_EXEC"]; next }
        /^Icon=/ { print "Icon=" icon; next }
        { print }
    ' "$entry_template" >"$1"
}
