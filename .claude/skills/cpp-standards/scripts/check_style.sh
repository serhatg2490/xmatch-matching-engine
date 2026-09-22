#!/usr/bin/env bash
# xmatch biçim kontrolü. Kural numaraları cpp-standards/SKILL.md ile,
# "bp-N" referansları best-practices/references/practices.md ile eşleşir.
#
# Kullanım:
#   check_style.sh              # git'e göre değişen C/C++ dosyaları
#   check_style.sh <dosya>...   # verilen dosyalar
# Çıkış: ihlal varsa 1, yoksa 0.

set -uo pipefail

MAX_COLS=110
repo_root=$(git rev-parse --show-toplevel 2>/dev/null) || repo_root=$PWD
cd "$repo_root" || exit 1

if [ $# -gt 0 ]; then
    files=("$@")
else
    mapfile -t files < <(
        { git diff --name-only --diff-filter=ACM HEAD
          git ls-files --others --exclude-standard; } 2>/dev/null \
        | grep -E '\.(cpp|hpp|c|h)$' | sort -u
    )
fi

violations=0
report() { echo "$1:$2: [$3] $4"; violations=$((violations + 1)); }

# Yalnızca yorumdan ibaret satırları eler ("// ...", "* ...", "/* ...").
# Satır sonundaki yorumlar elenmez — kasıtlı: kod tarafı yine taranır.
drop_comments() { grep -vE '^[0-9]+:[[:space:]]*(//|\*|/\*)'; }

for f in "${files[@]:-}"; do
    [ -f "$f" ] || continue
    case "$f" in *.cpp|*.hpp|*.c|*.h) ;; *) continue ;; esac
    # Yolu depoya göreli hale getir: aşağıdaki src/* ve include/* kalıpları
    # buna bağlı, hook ise mutlak yol gönderir.
    f=$(realpath --relative-to="$repo_root" "$f" 2>/dev/null || echo "$f")

    # Kural 1 — tab yok
    while IFS=: read -r n _; do
        [ -n "$n" ] && report "$f" "$n" "kural 1" "tab karakteri; 4 boşluk kullan"
    done < <(grep -nP '\t' "$f" 2>/dev/null)

    # Kural 2 — satır uzunluğu (tests/ muaf)
    case "$f" in tests/*) ;; *)
        while read -r n len; do
            report "$f" "$n" "kural 2" "satır $len kolon, sınır $MAX_COLS"
        done < <(awk -v m="$MAX_COLS" 'length>m{print NR, length}' "$f")
    ;; esac

    # Kural 3 — header koruması
    case "$f" in *.hpp|*.h)
        grep -q '^#pragma once' "$f" || report "$f" 1 "kural 3" "#pragma once yok"
        n=$(grep -nE '^#ifndef +[A-Z_]+_H' "$f" | head -1 | cut -d: -f1)
        [ -n "$n" ] && report "$f" "$n" "kural 3" "include guard; #pragma once kullan"
    ;; esac

    # Kural 5 — namespace / extern "C" kapanış yorumu
    for pair in 'namespace:^namespace .*\{:^\} // namespace' 'extern "C":^extern "C" \{:^\} // extern "C"'; do
        what=${pair%%:*}; rest=${pair#*:}; open=${rest%%:*}; close=${rest#*:}
        o=$(grep -cE "$open" "$f"); c=$(grep -cE "$close" "$f")
        [ "$o" -ne "$c" ] && report "$f" 1 "kural 5" \
            "$what: $o açılış / $c kapanış yorumu — '} // $what ...' ekle"
    done

    # Kural 7 — C-style cast
    while IFS=: read -r n _; do
        [ -n "$n" ] && report "$f" "$n" "kural 7" "C-style cast; static_cast kullan"
    done < <(grep -nP '\(\s*(int|unsigned|char|long|short|float|double)\s*\)\s*[A-Za-z_(]' "$f" | drop_comments)

    # Kural 9 — derleyici eklentisi
    while IFS=: read -r n _; do
        [ -n "$n" ] && report "$f" "$n" "kural 9" "__builtin_*; <bit> karşılığını kullan"
    done < <(grep -n '__builtin_' "$f" | drop_comments)

    # --- best-practices maddelerinin mekanik olarak ölçülebilen kısmı ---
    case "$f" in src/*|include/*)

        # bp-7 — sıcak yolda ayırma / syscall / log
        while IFS=: read -r n _; do
            [ -n "$n" ] && report "$f" "$n" "bp-7" "sıcak yolda I/O veya ayırma çağrısı"
        done < <(grep -nE '\b(printf|malloc|free|std::cout|std::cerr)\b' "$f" | drop_comments)

        # bp-14 — fiyat matematiğinde float yok (load-factor hesabı muaf)
        case "$f" in *flat_hash_map.hpp) ;; *)
            while IFS=: read -r n _; do
                [ -n "$n" ] && report "$f" "$n" "bp-14" "float/double; Price tamsayı aritmetiği kullan"
            done < <(grep -nE '\b(float|double)\b' "$f" | drop_comments)
        ;; esac

        # bp-16 — catch (...) yalnızca ABI sınırında
        case "$f" in src/engine.cpp|src/engine_api.cpp) ;; *)
            while IFS=: read -r n _; do
                [ -n "$n" ] && report "$f" "$n" "bp-16" "catch (...) yalnızca API sınırında olur"
            done < <(grep -n 'catch (\.\.\.)' "$f" | drop_comments)
        ;; esac

    ;; esac
done

if [ "$violations" -gt 0 ]; then
    echo "--- $violations ihlal. Kurallar: .claude/skills/cpp-standards/SKILL.md" >&2
    exit 1
fi
exit 0
