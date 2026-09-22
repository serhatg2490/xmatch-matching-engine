# Pratikler

Her madde: kural → neden → kötü/iyi → projeden çapa.
Örnekler farkı göstermek içindir, derlenebilir olmaları gerekmez.

## Tasarım ilkeleri

### 1. Public arayüzü dar tut

Dışarı açtığın her sembol kalıcı bir sözleşmedir.

```cpp
OrderPool* xmatch_get_pool(Engine*);              // kötü — iç tip sızıyor
IMatchingEngine* xmatch_create(IEventListener*);  // iyi  — opak pointer
```

Projede: `include/xmatch/matching_engine_api.hpp` — kütüphane üç C sembolü arkasında.

### 2. Her tip tek bir sorumluluk taşısın

Birleşik tipler ayrı ayrı test edilemez ve değiştirilemez.

```cpp
class OrderBook { Price index_to_price(...); OrderRecord* allocate(); };  // kötü
class PriceLadder { /* fiyat <-> indeks */ };   // iyi — üç ayrı bina taşı
class OrderPool   { /* slot ayırma      */ };
class OrderBook   { /* seviyeler, FIFO  */ };
```

Projede: `include/engine/` — `price_ladder` / `order_pool` / `order_book` ayrımı.

### 3. Tek kaynak ilkesi

Aynı bilgi iki yerde tanımlanırsa er ya da geç biri güncellenmeden kalır.

```cpp
Price breaks[] = {200000, 500000, 1000000};        // kötü — tablonun ikinci kopyası
for (const TickBracket& b : kTickSchedule) { ... } // iyi  — tek tabloyu dolaş
```

Projede: `include/engine/tick_table.hpp` tek kaynak, `src/price_ladder.cpp` onu
dolaşır. Bu ayrışma gerçek bir bug'dı (commit `d6627e0`).

### 4. Sözleşmeyi derleme zamanına taşı

Derleyicinin yakalayabileceği hatayı çalışma zamanına bırakma.

```cpp
// OrderRecord 48 bayt olmalı, dokümanlar buna dayanıyor   // kötü — temenni
static_assert(sizeof(OrderRecord) == 48, "see DESIGN.md"); // iyi  — derleme kırılır
```

Projede: `include/engine/order_pool.hpp:37`, `include/engine/order_book.hpp:27`.

## Performans ve ölçeklenebilirlik

### 5. Önce ölç, sonra optimize et

Tahmine dayalı optimizasyon kodu karmaşıklaştırır, kazancı çoğu zaman sıfırdır.

```cpp
// kötü: "elle açarsam hızlanır" varsayımı, ölçüm yok
// iyi : değişiklik öncesi/sonrası ölç, üretilen kodu doğrula, gerekçeyi yaz
//       "GCC 13 -O3'te bare bsr kalıyor, ek dal yok — objdump ile bakıldı"
```

Projede: `include/engine/level_bitset.hpp` — `<bit>` geçişi üretilen koda
bakılarak doğrulandı; yöntem ve rakamlar `BENCHMARK.md`'de.

### 6. Karmaşıklık mikro-optimizasyondan önce gelir

O(log n)'i O(1)'e indirmek, sabit çarpanla oynamaktan daha değerlidir.

```cpp
std::map<Price, Level> levels_;    // kötü — her erişim ağaç dolaşımı
std::vector<Level> levels_;        // iyi  — levels_[ladder_.price_to_index(p)]
```

Projede: `include/engine/order_book.hpp` — `price_to_index`, `add_resting`,
`top_of_book` hepsi O(1). Günlük fiyat bandı indeks kümesini sınırladığı için mümkün.

### 7. Sıcak yolda ayırma, syscall, kilit ve log yok

Tek bir allocation ya da syscall p99'u mikrosaniyelere taşır.

```cpp
void Engine::submit(...) { orders_.push_back(OrderRecord{}); }  // kötü — büyüyebilir
void Engine::configure(...) { pool_.reserve(kDefaultOrderCapacity); }  // iyi
void Engine::submit(...) { std::uint32_t slot = pool_.allocate(); }    //     indeks ilerletir
```

Projede: `src/engine.cpp` — `configure()` pool ve id indeksini önceden ayırır.

### 8. Veri yerleşimini düşün

Bitişik dizi taraması pointer takibinden kat kat hızlıdır; sık dokunulan
struct dar kalmalıdır.

```cpp
struct Level { std::list<Order*> orders; };   // kötü — her adım cache miss
struct Level { std::uint32_t head, tail; Quantity total_qty;
               std::uint32_t order_count; }; // iyi — 16 bayt, paddingsiz
```

Projede: `include/engine/order_book.hpp` — `Level` 16 bayt, iki düz dizi.

## Okunabilirlik

### 9. Erken dönüş kullan

İç içe koşullar okuma yükünü katlar ve sıra değiştirmeyi zorlaştırır.

```cpp
if (book) { if (!dup) { if (qty) {...} else reject(...); } else reject(...); }  // kötü

if (!book)   { reject(kUnknownInstrument); return; }   // iyi
if (dup)     { reject(kDuplicateOrderId);  return; }
if (qty == 0){ reject(kInvalidQuantity);   return; }
```

Projede: `src/engine.cpp` `submit()` — guard zinciri `RejectReason` enum sırasına
birebir eşlenir, böylece sözleşme koddan okunur.

### 10. İsim niyeti söylesin, sihirli sayı olmasın

Çıplak sabit okuyucuyu değerin nereden geldiğini aramaya zorlar.

```cpp
if (rec.next == 0xFFFFFFFFu) ...  pool_.reserve(16777216);              // kötü
if (rec.next == kInvalidSlot) ... pool_.reserve(kDefaultOrderCapacity); // iyi
```

Projede: `kInvalidSlot`, `kChunkSize` (`order_pool.hpp`), `kDefaultOrderCapacity`
(`engine.cpp`).

### 11. Bir fonksiyon tek soyutlama seviyesinde kalsın

Üst düzey akışla bit düzeyi detayı karıştırmak fonksiyonu test edilemez yapar.

```cpp
while (...) { std::uint64_t w = words_[i >> 6] & mask; ... }        // kötü
while (agg.open_qty > 0 && book.has_best(opp)) { ... }              // iyi
```

Projede: `src/engine.cpp` `run_matching()` yalnızca akışı taşır; kelime taraması
`LevelBitset` içinde kalır.

### 12. Yorum "neden"i açıklasın

"Ne yaptığı" koddan okunur; yorumun işi kodun söyleyemediğini söylemektir.

```cpp
// kelime sıfır değil mi diye bak                                    // kötü
// Guard çağrıdan ÖNCE kalmalı: GCC buradan w != 0'ı kanıtlayıp bare // iyi
// bsr üretiyor. Kaldırılırsa sıfır testi dal olarak geri geliyor.
if (w) return ...;
```

Projede: `include/engine/level_bitset.hpp` başlık yorumu.

## Güvenlik ve sağlamlık

### 13. Dış girdiye güvenme, sınırda doğrula

Doğrulama tek yerde ve belirli bir sırada olmalı, reddin sebebi çağırana bildirilmeli.

```cpp
book->add_resting(pool_, slot, o.side, o.price);      // kötü — doğrulanmamış girdi

if (!is_tick_aligned(price))       { reject(kInvalidPriceTick); return; }  // iyi
if (book->price_to_index(price)<0) { reject(kPriceOutOfBand);   return; }
```

Projede: `src/engine.cpp` — `submit()` ve `replace()` aynı sırayı uygular.

### 14. Tamsayı aritmetiğine dikkat et

Float para hesabında sessizce sapar; işaretli/işaretsiz karışımı sessizce sarar.

```cpp
double px = 12.34;  for (int i = 0; i < levels_.size(); ++i)          // kötü
Price px = 123400;  for (std::size_t i = 0; i < levels_.size(); ++i)  // iyi
```

Projede: `Price` = `int64_t` @ 1/10000 birim; fiyat, tick ve band matematiğinde
`float`/`double` yok. (`flat_hash_map.hpp`'deki `kMaxLoadFactor` sıcak yolda
olmayan, fiyatla ilgisiz bir kapasite hesabıdır — kural fiyat matematiği içindir.)

### 15. İndeks erişiminde sınırı garanti et

İndeksin geçerliliği hesaplandığı yerde kanıtlanmalı, kullanıldığı yerde varsayılmamalı.

```cpp
levels_[ladder_.price_to_index(price)].total_qty += qty;   // kötü — -1 dönebilir

std::int64_t idx = ladder_.price_to_index(price);          // iyi — sentinel açık
if (idx < 0) { reject(kPriceOutOfBand); return; }
```

Projede: `src/price_ladder.cpp` — band veya tick ihlalinde `-1`; `submit()` bunu
resting'den önce kontrol eder.

### 16. Hatayı sessizce yutma

`catch (...)` hata yönetimi değil sınır koruyucusudur; içeride kullanılırsa bug gizler.

```cpp
void OrderBook::add_resting(...) { try { ... } catch (...) {} }   // kötü

IMatchingEngine* xmatch_create(IEventListener* l) {               // iyi
    try { return new detail::Engine(l); }
    catch (...) { return nullptr; }   // exception C sınırını geçemez
}
```

Projede: `src/engine_api.cpp` ve `detail::Engine`'in public metotları — başka
hiçbir yerde yok.
