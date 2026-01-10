set shell := ["bash", "-cu"]

arch := `uname -m`
base_libs := "-lpthread -lcurl -lsqlite3"
sources := "parson.c sds.c json_wrap.c sqlite_wrap.c botlib.c"
target := "mybot"

# Build the bot executable (default recipe)
build:
    libs="{{base_libs}}"; if [[ "{{arch}}" == aarch64* || "{{arch}}" == armv* ]]; then libs="$libs -latomic"; fi; \
    cflags="-g -ggdb -O2 -Wall -Wextra -Wpedantic -Werror -std=c2x"; \
    ${CC:-cc} $cflags {{sources}} mybot.c -o {{target}} $libs

# Build with sanitizers enabled for debugging
build-sanitize:
    libs="{{base_libs}}"; if [[ "{{arch}}" == aarch64* || "{{arch}}" == armv* ]]; then libs="$libs -latomic"; fi; \
    cflags="-g -ggdb -O1 -Wall -Wextra -Wpedantic -Werror -std=c2x -fsanitize=address,undefined,leak -fno-omit-frame-pointer"; \
    ${CC:-cc} $cflags {{sources}} mybot.c -o {{target}} $libs

clean:
    rm -rf {{target}} zig-out .zig-cache mybot

format:
    clang-format -i *.c *.h

fmt: format
