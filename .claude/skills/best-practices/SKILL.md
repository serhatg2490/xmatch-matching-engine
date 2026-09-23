---
name: best-practices
description: xmatch'te iyi programlama pratikleri — tasarım ilkeleri, performans ve ölçeklenebilirlik, okunabilirlik, güvenlik ve sağlamlık. C++ kodu yazarken veya düzenlerken (.cpp/.hpp), refactor ederken, yeni bir modül/sınıf eklerken, code review yaparken ya da bir tasarım kararını gerekçelendirirken kullan. Biçim kuralları (girinti, isimlendirme, cast, include sırası) bu skill'in kapsamı değildir — onlar cpp-standards'a aittir.
disable-model-invocation: true
allowed-tools: Read
---

# İyi programlama pratikleri

C++ kodu yazarken, refactor ederken veya gözden geçirirken kararları şu dört
başlık altında ver:

1. **Tasarım ilkeleri** — sorumluluk dağılımı, arayüz genişliği, bilginin
   nerede tanımlandığı.
2. **Performans ve ölçeklenebilirlik** — karmaşıklık, sıcak yol maliyeti,
   veri yerleşimi, ölçüme dayalı karar.
3. **Okunabilirlik** — kontrol akışı, isimlendirme, soyutlama seviyesi,
   yorumun işlevi.
4. **Güvenlik ve sağlamlık** — girdi doğrulama, tamsayı aritmetiği, sınır
   güvenliği, hata görünürlüğü.

## Nasıl kullanılır

Maddelerin tamamı ve her biri için kötü/iyi örnekler
[`references/practices.md`](references/practices.md) dosyasındadır.
Bir pratiği uygularken, bir tasarım kararını tartarken veya kod gözden
geçirirken **önce o dosyayı oku**, sonra ilgili maddeyi numarasıyla gerekçe
göster.

Bir madde ile bu projenin `CLAUDE.md`'sindeki invariant'lar çelişirse
`CLAUDE.md` kazanır; bu dosya onu genelleştirir, yerine geçmez.
