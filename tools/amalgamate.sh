#!/usr/bin/env bash

set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
DIST_DIR="$ROOT/dist"
URB_HEADER="$ROOT/include/urb.h"
PUBLIC_HEADERS=(
    "$ROOT/include/urbc/common.h"
    "$ROOT/include/urbc/ffi_sig.h"
    "$ROOT/include/urbc/format.h"
    "$ROOT/include/urbc/runtime.h"
    "$ROOT/include/urbc/loader.h"
    "$ROOT/include/urbc/api.h"
)
INTERNAL_HEADER="$ROOT/src/urbc_internal.h"
SOURCES=(
    "$ROOT/src/urbc_format.c"
    "$ROOT/src/urbc_platform.c"
    "$ROOT/src/urbc_ffi_sig.c"
    "$ROOT/src/urbc_loader.c"
    "$ROOT/src/urbc_schema.c"
    "$ROOT/src/urbc_runtime.c"
    "$ROOT/src/urbc_ops_core.c"
    "$ROOT/src/urbc_ops_mem.c"
    "$ROOT/src/urbc_ops_schema.c"
    "$ROOT/src/urbc_ops_ffi.c"
    "$ROOT/src/urbc_api.c"
)

read -r -d '' CLI_TEMPLATE <<'EOF' || true
#include <stdio.h>
#include <string.h>

static void urbc_cli_print_value(const char *label, Value v)
{
    printf("%s i=%lld u=%llu f=%g p=%p\n",
           label,
           (long long)v.i,
           (unsigned long long)v.u,
           (double)v.f,
           v.p);
}

int main(int argc, char **argv)
{
    UrbcImage image;
    UrbcRuntime rt;
    char err[URBC_ERROR_CAP];
    int dump_stack = 0;
    int argi = 1;
    UHalf i;

    if (argc < 2) {
        fprintf(stderr, "usage: %s [--stack] file.uffi\n", argv[0]);
        return 1;
    }
    if (argi < argc && strcmp(argv[argi], "--stack") == 0) {
        dump_stack = 1;
        argi++;
    }
    if (argi >= argc) {
        fprintf(stderr, "usage: %s [--stack] file.uffi\n", argv[0]);
        return 1;
    }

    urbc_image_init(&image);
    urbc_runtime_init(&rt);

    if (urbc_load_from_file(argv[argi], &image, err, sizeof(err)) != URBC_OK) {
        fprintf(stderr, "load failed: %s\n", err);
        urbc_runtime_destroy(&rt);
        urbc_image_destroy(&image);
        return 1;
    }
    if (urbc_runtime_bind(&rt, &image, NULL, NULL, err, sizeof(err)) != URBC_OK) {
        fprintf(stderr, "bind failed: %s\n", err);
        urbc_runtime_destroy(&rt);
        urbc_image_destroy(&image);
        return 1;
    }
    if (urbc_runtime_run(&rt, err, sizeof(err)) != URBC_OK) {
        fprintf(stderr, "run failed: %s\n", err);
        urbc_runtime_destroy(&rt);
        urbc_image_destroy(&image);
        return 1;
    }

    printf("stack_size=%u\n", (unsigned)rt.stack->size);
    if (rt.stack->size > 0)
        urbc_cli_print_value("top", rt.stack->data[rt.stack->size - 1]);
    if (dump_stack) {
        for (i = 0; i < rt.stack->size; i++) {
            char label[32];
            snprintf(label, sizeof(label), "stack[%u]", (unsigned)i);
            urbc_cli_print_value(label, rt.stack->data[i]);
        }
    }

    urbc_runtime_destroy(&rt);
    urbc_image_destroy(&image);
    return 0;
}
EOF

relative_path() {
    local path="$1"
    printf '%s\n' "${path#"$ROOT"/}"
}

trim_blank_edges() {
    awk '
        {
            lines[++count] = $0
        }
        END {
            start = 1
            while (start <= count && lines[start] ~ /^[[:space:]]*$/) {
                start++
            }

            end = count
            while (end >= start && lines[end] ~ /^[[:space:]]*$/) {
                end--
            }

            for (i = start; i <= end; i++) {
                print lines[i]
            }
        }
    '
}

strip_local_includes() {
    awk '
        {
            stripped = $0
            sub(/^[[:space:]]+/, "", stripped)
            sub(/[[:space:]]+$/, "", stripped)

            if (stripped ~ /^#include[[:space:]]+"urbc\/[^"]+"[[:space:]]*$/ ||
                stripped == "#include \"urbc.h\"" ||
                stripped == "#include \"urbc_internal.h\"" ||
                stripped == "#include \"urb.h\"") {
                next
            }

            print
        }
    '
}

strip_header_guard() {
    local file="$1"

    awk '
        {
            lines[++count] = $0
        }
        END {
            guard_idx = 0
            define_idx = 0

            for (i = 1; i <= count; i++) {
                stripped = lines[i]
                sub(/^[[:space:]]+/, "", stripped)
                sub(/[[:space:]]+$/, "", stripped)

                if (guard_idx == 0 && stripped ~ /^#ifndef[[:space:]]+/) {
                    guard_idx = i
                    continue
                }

                if (guard_idx != 0 && define_idx == 0 && stripped ~ /^#define[[:space:]]+/) {
                    define_idx = i
                    break
                }

                if (stripped != "" && stripped !~ /^\/\// && stripped !~ /^\/\*/ && stripped !~ /^\*/) {
                    break
                }
            }

            start = 1
            if (guard_idx != 0 && define_idx != 0) {
                start = define_idx + 1
            }

            end = count
            while (end >= start && lines[end] ~ /^[[:space:]]*$/) {
                end--
            }

            if (end >= start) {
                stripped = lines[end]
                sub(/^[[:space:]]+/, "", stripped)
                sub(/[[:space:]]+$/, "", stripped)
                if (stripped ~ /^#endif([[:space:]]|$)/) {
                    end--
                }
            }

            for (i = start; i <= end; i++) {
                print lines[i]
            }
        }
    ' "$file"
}

process_header() {
    local file="$1"
    strip_header_guard "$file" | strip_local_includes | trim_blank_edges
}

process_source() {
    local file="$1"
    strip_local_includes < "$file" | trim_blank_edges
}

emit_section() {
    local title="$1"

    printf '\n/* ===== %s ===== */\n\n' "$title"
    cat
    printf '\n'
}

build_urbc_h() {
    local output="$DIST_DIR/urbc.h"
    local tmp
    local file

    mkdir -p "$DIST_DIR"
    tmp="$(mktemp)"

    {
        printf '/* Generated by tools/amalgamate.sh. Do not edit directly. */\n'
        printf '#ifndef URBC_H\n'
        printf '#define URBC_H 1\n'

        process_header "$URB_HEADER" | emit_section 'include/urb.h'

        for file in "${PUBLIC_HEADERS[@]}"; do
            process_header "$file" | emit_section "$(relative_path "$file")"
        done

        printf '\n#ifdef URBC_IMPLEMENTATION\n'
        printf '#ifndef URBC_IMPLEMENTATION_ONCE\n'
        printf '#define URBC_IMPLEMENTATION_ONCE 1\n'

        process_header "$INTERNAL_HEADER" | emit_section "$(relative_path "$INTERNAL_HEADER")"

        for file in "${SOURCES[@]}"; do
            process_source "$file" | emit_section "$(relative_path "$file")"
        done

        printf '#endif /* URBC_IMPLEMENTATION_ONCE */\n'
        printf '#endif /* URBC_IMPLEMENTATION */\n\n'
        printf '#endif /* URBC_H */\n'
    } > "$tmp"

    mv "$tmp" "$output"
}

build_urbccli_c() {
    local output="$DIST_DIR/urbccli.c"
    local tmp

    mkdir -p "$DIST_DIR"
    tmp="$(mktemp)"

    {
        printf '/* Generated by tools/amalgamate.sh. Do not edit directly. */\n'
        printf '#define URBC_IMPLEMENTATION 1\n'
        printf '#include "urbc.h"\n\n'
        printf '%s\n' "$CLI_TEMPLATE"
    } > "$tmp"

    mv "$tmp" "$output"
}

main() {
    build_urbc_h
    build_urbccli_c
    printf 'generated dist/urbc.h and dist/urbccli.c\n'
}

main "$@"