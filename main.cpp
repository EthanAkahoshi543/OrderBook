#include "OrderBook.hpp"

#include <cstdio>

using namespace lob;

int main()
{
    OrderBook book;
    book.Submit({ 1, Side::Sell, 101, 5 });
    book.Submit({ 2, Side::Sell, 102, 5 });
    book.Submit({ 3, Side::Buy, 99, 10, Tif::GoodForDay });

    for (const Fill& f : book.Submit({ 4, Side::Buy, 102, 7 }))
        std::printf("fill: taker %llu <- maker %llu  %u @ %lld\n", (unsigned long long)f.taker, (unsigned long long)f.maker, f.qty, (long long)f.price);

    std::printf("orders resting: %zu, best bid %lld, best ask %lld\n", book.Size(), (long long)*book.BestBid(), (long long)*book.BestAsk());
    std::printf("expired at close: %zu\n", book.ExpireDayOrders());
}
