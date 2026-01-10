set shell := ["bash", "-cu"]

arch := `uname -m`
base_libs := "-lpthread -lcurl -lsqlite3"
sources := "parson.c sds.c json_wrap.c sqlite_wrap.c botlib.c"
target := "mybot"

# Build the bot executable (default recipe)
build:
    libs="{{base_libs}}"; if [[ "{{arch}}" == aarch64* || "{{arch}}" == armv* ]]; then libs="$libs -latomic"; fi; \
    ${CC:-cc} -g -ggdb -O2 -Wall -Wextra -Wpedantic -Werror -std=c2x {{sources}} mybot.c -o {{target}} $libs

clean:
    rm -f {{target}}
