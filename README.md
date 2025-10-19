# High-Performance Multi-threaded HTTP Server (C)

![Language](https://img.shields.io/badge/language-C-blue)
![License](https://img.shields.io/badge/license-MIT-green)
![Build](https://img.shields.io/badge/build-passing-brightgreen)

This project implements a **lightweight, multi-threaded HTTP server** in C, designed for performance testing and concurrent request handling using **epoll** and **POSIX threads (pthreads)**.

---

## Overview

`http_server` is a simple yet efficient HTTP server that:
- Handles multiple client connections concurrently using epoll.
- Distributes connections across multiple worker threads (round-robin).
- Supports real-time statistics and debug toggling.
- Allows configurable artificial delay and error rate simulation.
- Demonstrates system-level network programming and concurrency in C.

---

## Features

- **Multi-threaded worker pool** using pthreads  
- **Non-blocking I/O** with `epoll`  
- **Dynamic connection assignment** (round-robin)  
- **Atomic counters** for active connections and request tracking  
- **Real-time logging** of requests per second  
- **Debug mode toggle** with a single keypress (`d`)  
- **Customizable options**:
  - Add artificial latency (`delay=<ms>`)
  - Simulate HTTP error responses at a given rate (`errate=<0–1>`)
  - Run in background mode (`bg`)

---

## Supported HTTP Responses

| Status Code | Description | Content-Type | Example Use |
|--------------|-------------|---------------|--------------|
| 200 OK | Normal success response | text/plain | Default reply ("Hello, world!") |
| 400 Bad Request | Invalid request format | application/json | Input errors |
| 401 Unauthorized | Missing/invalid auth | application/json | Token validation test |
| 403 Forbidden | Access denied | application/json | Security tests |
| 404 Not Found | Resource missing | application/json | URL not found |
| 500 Internal Server Error | Simulated server error | application/json | Stress testing |
| 503 Service Unavailable | Overload simulation | application/json | Error rate testing |

---

## Build Instructions

### Prerequisites
- GCC compiler  
- Linux system (for `epoll` and `pthread`)  
- Optional: `curl` for testing

### Build
```bash
gcc -o http_server http_server.c -pthread -std=c11
```

### Run
```bash
./http_server <port> [options]
```

#### Example:
```bash
./http_server 8080 delay=200 errate=0.1
```

This runs the server on port `8080`, adds a 200ms delay to each response, and sends an error response every 10th request (10% error rate).

#### Options

| Option | Description |
|--------|-------------|
| `delay=<ms>` | Add artificial delay in milliseconds (0–5000). |
| `errate=<rate>` | Simulate errors with a probability (0–1). |
| `bg` | Run in background mode (disables stdin input). |

---

## Testing

Use `curl` to test the server locally:
```bash
curl -v -X GET http://localhost:8080
```

Example output:
```
HTTP/1.1 200 OK
Content-Length: 13
Connection: keep-alive
Content-Type: text/plain

Hello, world!
```

---

## Monitoring

The server prints request statistics every 10 seconds:
```
[2025-10-18 18:30:00] Requests received[4]: 120, Msg per Second: 12.00
```

Press **`d`** during runtime to toggle **debug mode** for detailed request logging.

---

## Architecture Overview

- **Main Thread** — Accepts incoming TCP connections and distributes them among worker threads.  
- **Worker Threads** — Each uses an independent epoll instance to handle read events from assigned sockets.  
- **Stats Thread** — Prints request/connection metrics every 10 seconds.  
- **Debug Thread** — Listens for key input to toggle debug mode.  

---

## Example Use Cases

- Benchmarking and load testing HTTP clients or proxies  
- Simulating backend service failures using controlled error rates  
- Learning Linux system programming with threads, sockets, and epoll  

---

## Future Improvements

- Add HTTPS (TLS) support  
- Implement HTTP keep-alive timeout and proper connection close handling  
- Add JSON configuration file for runtime parameters  
- Support POST/PUT/DELETE methods  

---

## Contributing

Contributions are welcome!  
Feel free to open issues or submit pull requests to improve functionality or documentation.

---

## License

This project is open-source under the **MIT License**.  
Feel free to use, modify, and distribute it for educational or benchmarking purposes.
