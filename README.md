# misc

`misc` contains small standalone utilities that do not belong to a larger project.

Current contents:

- `runtelnet.c` - small Telnet command runner, currently Version 1.02;
- `Makefile` - portable build file for `runtelnet`;
- `runtelnet` - locally built executable.

## runtelnet

`runtelnet` opens a Telnet connection to a host, performs the login sequence, sends one or more commands and waits for the remote prompt between commands.

It was written primarily for MikroTik RouterOS, but the network connection itself accepts either IPv4 or IPv6 addresses or host names through `getaddrinfo()`.

The program uses TCP port 23.

### Syntax

```sh
runtelnet host user password "command" ["command" ...]
```

Example:

```sh
./runtelnet 2001:db8::1 admin password \
    "/system resource print" \
    "/interface print" \
    "/quit"
```

Each command is a separate command-line argument. Quoting each RouterOS command is recommended so spaces and shell metacharacters are not interpreted by the local shell.

There is deliberately no special separator character inside the command string.

### Session sequence

The current sequence is:

1. connect to TCP port 23;
2. wait for the `login:` prompt;
3. send the user name;
4. wait for the `password:` prompt;
5. send the password;
6. wait for the RouterOS command prompt;
7. send each command in command-line order;
8. wait for the RouterOS prompt before sending the next command.

`/quit` is not inserted automatically. If a clean RouterOS logout is wanted, pass `/quit` explicitly as the last command:

```sh
./runtelnet router admin password "/system resource print" "/quit"
```

This keeps every remote action visible in the command line and allows the caller to decide exactly which commands are sent.

If the final command is `/quit`, the remote side is expected to close the connection and that is treated as normal completion.

### Telnet protocol handling

`runtelnet` implements the small amount of Telnet negotiation required for this use case. It recognizes Telnet IAC negotiation sequences and refuses unsupported options instead of passing negotiation bytes to the prompt parser.

It does not depend on an external `telnet` executable.

### Output

Login and password prompts are consumed internally. Command output received after login is written to standard output.

Errors are written to standard error and the program returns a non-zero exit status on connection, protocol, prompt, send or receive failures.

### Timeout

The current receive timeout is 15 seconds for each wait operation.

This is intended to avoid leaving the program blocked indefinitely if the remote system does not answer or if the expected prompt is not received.

## Build

The program has no third-party library dependencies and uses the normal POSIX socket interfaces.

Build with:

```sh
make
```

Clean with:

```sh
make clean
```

The default build is equivalent to:

```sh
cc -O2 -Wall -Wextra -std=gnu89 -o runtelnet runtelnet.c
```

A different compiler can be selected normally through `make`, for example:

```sh
make CC=clang
```

Compiler flags can also be replaced by the caller:

```sh
make CFLAGS="-O3 -Wall -Wextra -std=gnu89"
```

The Makefile intentionally uses the standard `CC`, `CFLAGS`, `CPPFLAGS`, `LDFLAGS` and `LDLIBS` variables to make rebuilding on other Unix-like platforms straightforward.

## C programming style

Programs in this directory follow the project C conventions:

- C89-style source;
- build in `gnu89` mode because `//` comments are required;
- variables declared at the beginning of functions or blocks;
- variables of the same type grouped where readable and semantically appropriate;
- initialization after declaration;
- prefer `for` to `while` where practical;
- comments in English and only with `//`;
- prefer standard library and system functions over unnecessary helpers;
- performance and memory use are design constraints;
- no unused variables, functions or dead code;
- opening brace on the same line as the statement, with one space before `{`;
- source release number is incremented for every effective code change.

Source headers use the form:

```c
// Gianluca Mazzini @2026- Version x.yy
```
