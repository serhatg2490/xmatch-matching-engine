---
name: cpp-standards
description: xmatch C/C++ biçim ve mekanik kodlama standardı — girinti, satır uzunluğu, include guard ve sırası, namespace kapanışı, isimlendirme, cast, tamsayı tipleri. .cpp/.hpp/.c/.h dosyası yazarken veya düzenlerken, yeni dosya eklerken ve code review'da uygula. Tasarım/performans/güvenlik ilkeleri bu skill'in kapsamı değildir — onlar best-practices skill'ine aittir.
---

# C/C++ kodlama standardı

Kurallar mevcut koddan türetildi; kod tabanı bugün hepsini geçiyor.

## Biçim

1. Girinti 4 boşluk. Tab yok.
2. Satır ≤ 110 kolon. (`tests/` muaf: GoogleTest makroları doğal olarak uzun.)
3. Header'lar `#pragma once` ile korunur; `#ifndef` include guard kullanılmaz.
4. Include sırası — gruplar arası bir boş satır:
   kendi header'ı (`.cpp`'de) → `<standart>` → `"xmatch/..."` → `"engine/..."`.
5. `namespace` ve `extern "C"` blokları kapanış yorumuyla biter:
   `} // namespace xmatch::detail`, `} // extern "C"`.

## İsimlendirme

6. Tip `PascalCase`, fonksiyon ve değişken `snake_case`, üye alan sonda `_`,
   `constexpr` sabitler ve enum değerleri `kCamelCase`. Makro yazılmaz.

## Tip ve dönüşüm

7. C-style cast yok; `static_cast` kullanılır. `reinterpret_cast` ve
   `const_cast` ancak gerekçeyi yazan bir yorumla.
8. Saklanan ya da arayüzden geçen her tamsayı sabit genişlikli olur
   (`std::uint32_t`, `std::int64_t`, `std::size_t`). Kısa ömürlü yerel bir
   kaydırma sayacı için çıplak `int` kabul edilebilir.
9. Derleyici eklentisi yerine standart kütüphane: `__builtin_clzll` değil
   `std::countl_zero` (`<bit>`).
10. Trivial olmayan parametreler `const T&` ile geçilir; mutasyon yapmayan
    metotlar `const` işaretlenir.

## Kontrol

```sh
.claude/skills/cpp-standards/scripts/check_style.sh          # değişen dosyalar
.claude/skills/cpp-standards/scripts/check_style.sh <dosya>  # tek dosya
```

Script 1, 2, 3, 5, 7, 9 numaralı kuralları mekanik olarak doğrular. 4, 6, 8, 10
regex'le güvenilir biçimde ayrılamaz (yanlış pozitif üretir) — onlar yazarken ve
review'da elle uygulanır.

Script ayrıca `best-practices` skill'inin 7, 14 ve 16 numaralı maddelerini,
mekanik olarak ölçülebildikleri ölçüde kontrol eder ve ihlalde o madde
numarasını gösterir. Maddelerin metni burada tekrarlanmaz; gerekçe için
`best-practices/references/practices.md` okunur.
