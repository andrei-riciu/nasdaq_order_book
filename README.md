# nasdaq_order_book

ITCH 5.0 feed decoder and order book in C++ with Python analytics.

> Work in progress.

## Build

Requires a C++23 compiler (GCC 13+ / Clang 17+) and CMake 3.24+.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Debug builds enable AddressSanitizer and UBSan:

```sh
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j && ctest --test-dir build-debug
```

## Data

Sample files are published by Nasdaq at <https://emi.nasdaq.com/ITCH/> and are not
included in this repo. Place them in `data/`:

```sh
mkdir -p data && cd data
curl -LO 'https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/10302019.NASDAQ_ITCH50.gz'
```

Protocol reference: [Nasdaq TotalView-ITCH 5.0 specification](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf).
