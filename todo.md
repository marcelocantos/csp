# TODO

## Entropy / random parts

Add parts that produce random data as streams:

- `randint<T>(lo, hi)` — uniform random integers in [lo, hi)
- `random_bytes(n)` — chunks of random bytes (vector<uint8_t>)
- Fast PRNG source (e.g. xoshiro256**, PCG) — seedable, reproducible
- Crypto-quality PRNG source (e.g. libsodium randombytes, OpenSSL RAND)
- `/dev/urandom` / `/dev/random` reader via `byte_reader` + framing
- Configurable seeding (fixed seed for reproducibility vs entropy-seeded)
