// ============================================================================
//  xmatch — public API for the matching engine shared library
//  File: xmatch/matching_engine_api.hpp
//  API version: 1
//
//  Copyright (c) 2025 Serhat Gul. Released under the MIT License; see LICENSE.
//
//  The library is built as libmatching_engine.so and is meant to be loaded
//  either by linking against it directly or with dlopen(). In the dlopen case
//  the three extern "C" entry points at the bottom of this file are the only
//  symbols a host program needs to resolve:
//
//      xmatch_api_version, xmatch_create, xmatch_destroy
//
//  Requirements: C++20, Linux x86-64, GCC >= 12 or Clang >= 15.
//
//  Concurrency model
//    The engine is single-threaded. Every IMatchingEngine method is expected
//    to be called from one and the same thread, and the engine performs no
//    internal locking.
//
//  Event delivery
//    IEventListener callbacks are invoked synchronously, on the caller's
//    thread, and all of them have been delivered by the time the triggering
//    submit()/cancel()/replace() call returns. There is no queue and no
//    deferred dispatch, so a caller can time an operation simply by wrapping
//    the call.
//
//  Determinism
//    The same configure() followed by the same sequence of operations always
//    produces the same sequence of events, with identical field values, on
//    every run. Nothing in the engine depends on wall-clock time, addresses,
//    hash iteration order, or uninitialized memory.
// ============================================================================

#pragma once

#include <cstddef>
#include <cstdint>

namespace xmatch {

// ---------------------------------------------------------------------------
// Scalar types
//
// Prices are fixed-point integers with four implied decimals, so one unit is
// 0.0001 of the quote currency and 25.48 is carried as 254800. Integers are
// used throughout: no price or band arithmetic anywhere in the engine goes
// through float or double.
// ---------------------------------------------------------------------------
using Price = std::int64_t;     // 1 unit = 0.0001 currency units
using Quantity = std::uint32_t; // lots; this engine treats 1 lot as 1 share
using OrderId = std::uint64_t;  // assigned by the caller, unique per engine
using TradeId = std::uint64_t;  // assigned by the engine, 1, 2, 3, ...
using InstrumentId = std::uint32_t;

inline constexpr std::uint32_t kApiVersion = 1;
inline constexpr Price kPriceScale = 10000; // price units per 1.00

enum class Side : std::uint8_t {
    kBuy = 0,
    kSell = 1,
};

enum class OrderType : std::uint8_t {
    kLimit = 0,
    kMarket = 1, // the price field is unused; tif must be kIoc or kFok
};

enum class TimeInForce : std::uint8_t {
    kDay = 0, // remainder rests on the book until filled or canceled
    kIoc = 1, // take what is available now, cancel the rest
    kFok = 2, // fill the whole order at once or execute nothing
};

// Why a submit(), cancel() or replace() call was refused. Validation stops at
// the first problem found, in the order listed here.
enum class RejectReason : std::uint8_t {
    kUnknownInstrument = 0,  // instrument_id was never passed to configure()
    kDuplicateOrderId = 1,   // this order_id was already used
    kInvalidQuantity = 2,    // quantity is zero
    kInvalidPriceTick = 3,   // price does not sit on a tick boundary
    kPriceOutOfBand = 4,     // price falls outside the instrument's daily band
    kInvalidTimeInForce = 5, // e.g. a market order with tif == kDay
    kUnknownOrder = 6,       // no open order with the given id
};

// Why an order, or what was left of it, left the book.
enum class CancelReason : std::uint8_t {
    kUserRequested = 0, // an explicit cancel(), or a replace down to zero
    kIocRemainder = 1,  // the part of an IOC order that found no counterparty
    kFokUnfillable = 2, // an FOK order that could not fill in full; no trades
    kNoLiquidity = 3,   // a market order ran out of book on the opposite side
};

// ---------------------------------------------------------------------------
// Instrument configuration
//
// Each instrument trades inside a daily band centred on a reference price:
// [reference_price * (1 - band_bps/10000), reference_price * (1 + band_bps/10000)].
// Both ends are pulled inward onto a valid tick, so the lower limit rounds up
// and the upper limit rounds down. Limit prices outside the band are rejected;
// market orders are not band-checked. The tick ladder itself lives inside the
// engine (see include/engine/tick_table.hpp).
// ---------------------------------------------------------------------------
struct InstrumentConfig {
    InstrumentId instrument_id;
    Price reference_price;  // tick-aligned base price, e.g. the previous close
    std::uint32_t band_bps; // 1000 means the band spans +/- 10%
};

struct NewOrder {
    OrderId order_id; // caller-assigned, must not repeat within one engine
    InstrumentId instrument_id;
    Side side;
    OrderType type;
    TimeInForce tif;
    Price price;       // unused when type == kMarket
    Quantity quantity; // must be greater than zero
};

struct CancelOrder {
    OrderId order_id; // the open order to withdraw
};

// A replace changes an open order's price, its quantity, or both, in a single
// step; there is no window in which the order is off the book. The rules:
//
//   * new_order_id must be an id that has not been used yet, and the order
//     answers to that id once the replace succeeds.
//   * Raising the price, lowering it, or increasing the quantity sends the
//     order to the back of the queue at its price level. Reducing quantity
//     while leaving the price alone keeps its place in line.
//   * new_quantity counts the already-filled part, so the open quantity that
//     remains is max(0, new_quantity - filled). When that works out to zero
//     the order is simply canceled.
//   * The new price is checked against the tick table and the band again. If
//     it fails, the original order stays on the book exactly as it was.
//   * A new price that crosses the book matches right away, after the
//     on_replaced event has been delivered.
struct ReplaceOrder {
    OrderId order_id;      // the open order to modify
    OrderId new_order_id;  // the id it will carry afterwards
    Price new_price;
    Quantity new_quantity; // total, including whatever has already filled
};

struct TopOfBook {
    Price bid_price;       // only meaningful when bid_quantity > 0
    Quantity bid_quantity; // zero means there are no buy orders
    Price ask_price;       // only meaningful when ask_quantity > 0
    Quantity ask_quantity; // zero means there are no sell orders
};

// ---------------------------------------------------------------------------
// Event sink, implemented by the host application
//
// For a new order that passes validation the engine emits on_accepted first,
// then one on_trade per resting order it matched against, then, if anything is
// left over and cannot rest, an on_canceled.
// ---------------------------------------------------------------------------
class IEventListener {
public:
    virtual ~IEventListener() = default;

    // The order passed validation and has entered the engine. This arrives
    // before any fills the order goes on to generate.
    virtual void on_accepted(OrderId order_id) = 0;

    virtual void on_rejected(OrderId order_id, RejectReason reason) = 0;

    // One execution against one resting order. An aggressive order that
    // sweeps several resting orders produces one of these per resting order,
    // in the order they were matched. The price reported is always the
    // resting order's price.
    virtual void on_trade(TradeId trade_id,
                          InstrumentId instrument_id,
                          Price price,
                          Quantity quantity,
                          OrderId resting_order_id,
                          OrderId aggressive_order_id,
                          Side aggressor_side) = 0;

    // The order, or the part of it that had not filled, is gone from the book
    // and will not trade again.
    virtual void on_canceled(OrderId order_id,
                             Quantity canceled_quantity,
                             CancelReason reason) = 0;

    // A replace took effect. Delivered before any trades the repriced order
    // triggers.
    virtual void on_replaced(OrderId old_order_id,
                             OrderId new_order_id,
                             Price new_price,
                             Quantity new_open_quantity) = 0;
};

// ---------------------------------------------------------------------------
// The engine itself
// ---------------------------------------------------------------------------
class IMatchingEngine {
public:
    virtual ~IMatchingEngine() = default;

    // Declares the tradable instruments. Call this once, before anything
    // else; the engine copies what it needs and does not retain the pointer.
    // Only the first call takes effect: any later call is ignored and leaves
    // the configured instruments and the book untouched.
    virtual void configure(const InstrumentConfig* instruments,
                           std::size_t count) = 0;

    // Continuous-session order entry, matched on price then time: the better
    // price trades first, and among equal prices the order that arrived
    // earlier trades first.
    virtual void submit(const NewOrder& order) = 0;

    virtual void cancel(const CancelOrder& request) = 0;

    virtual void replace(const ReplaceOrder& request) = 0;

    // Best bid and best ask with their aggregate sizes. Runs in O(1).
    virtual TopOfBook top_of_book(InstrumentId instrument_id) const = 0;
};

} // namespace xmatch

// ---------------------------------------------------------------------------
// C entry points exported by the shared library
// ---------------------------------------------------------------------------
// The library is built with hidden default visibility, so these are the only
// symbols it exports.
#define XMATCH_EXPORT __attribute__((visibility("default")))

extern "C" {

// Returns xmatch::kApiVersion, so a host can check it matches this header.
XMATCH_EXPORT std::uint32_t xmatch_api_version();

// Builds an engine that reports to `listener`. The listener must be non-null
// and must stay alive for as long as the engine does. Never throws across
// this boundary; returns nullptr if the engine could not be created.
XMATCH_EXPORT xmatch::IMatchingEngine* xmatch_create(xmatch::IEventListener* listener);

XMATCH_EXPORT void xmatch_destroy(xmatch::IMatchingEngine* engine);

} // extern "C"
