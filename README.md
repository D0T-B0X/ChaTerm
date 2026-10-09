# ChaTerm

## How to Build

### Dependencies
- Linux/Unix environment (uses `epoll`, `pthread`, `wordexp`)
- GCC or Clang
- OpenSSL development libraries (for SHA-256 and random bytes during registration)
- CMake (if not compiling manually)

### Compilation (using CMake)

```bash
cmake -S . -B build
cmake --build build -j $(nproc)
./build/ChaTerm
```

Alternatively, compile manually:

```bash
# Server
gcc -o server server.c helper/*.c data/*.c -lpthread -lssl -lcrypto

# Client
gcc -o client client.c helper/UI/*.c -lpthread
```

## How to Run

### Start the server

```
./build/ChaTerm
```

Then, select `Server` on the menu. The server will bind to a local IP and port 5713. The address and port are displayed in the terminal.

### Start the client

```
/build/ChaTerm
```

Select the `Client` on the menu, and enter the server's IP and port number to attemp a connection.

**Local data is stored in $HOME/.local/ChaTerm/users.dat**

## Cipher Choice

For this system, I chose the repeating XOR cipher, because of its ease of implementation and universal application which makes it ideal to use as a one size fits all cipher to encrypt any data.

## Known Weaknesses

If a user doesn't provide a full cipher (16 characters), the strength of the encryption is significantly reduced with each omission.

## AI Usage

The application's UI is entirely genarated using several models. The AI generated files include:

```
AppMenu  .h/.c
ClientUI .h/.c
ServerUI .h/.c
```

The API provided by the generated UI libraries have been used heavily throughout the program. They only provide cosmetic changes to the server response and client errors.

All other files have been designed manually, and written with only minor help for debugging and documentation purposes.

## Design Notes

* **Hop-by-hop (not end-to-end) encryption:** The server acts as a middleman, briefly holding the plaintext in memory to decrypt incoming packets with the sender's key and re-encrypt them with the receiver's key
* **File transfer size cap:** 1 MB. This strict limit prevents server memory exhaustion.
* **Concurrency model:** `epoll` with edge-triggering (`EPOLLET`) chosen because it highly optimizes non-blocking socket I/O. It allows the server to efficiently monitor thousands of concurrent connections without blocking.
* **Execution engine:** A `ThreadPool` that extracts tasks filled in a `TaskQueue` by the server executes heavy CPU bound operations concurrently on multiple CPU threads, this optimizes the main thread to focus on handling networking events.

## Known Limitations
* **True End-to-End Encryption:** Because this relies on hop-by-hop encryption, the server is fully aware of all plaintext messages. Achieving true end-to-end encryption would require clients to establish a shared secret directly with one another..
* **Key Storage:** Symmetric keys are stored in the server's memory/local storage mapping, meaning a server breach exposes all user keys.

# Build scripts

I have also included two build scripts in this repository `new.sh` and `run.sh`, where `new.sh` builds the project from the defined CMake and `run.sh` begins execution on top of compilation.

```bash
chmod +x new.sh run.sh
```

You can now use the build scripts to build and execute the project