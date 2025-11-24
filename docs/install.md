Here’s the *clean and correct* way to build and run PostgreSQL **from source** on macOS (including Apple Silicon like your M3).

This assumes you cloned the PostgreSQL repo (like `git clone https://github.com/postgres/postgres.git`).

---

# ✅ **1. Install Dependencies**

You need Xcode tools, Flex, Bison, and OpenSSL.

```bash
xcode-select --install
brew install bison flex readline openssl
```

You **must** tell PostgreSQL where OpenSSL is located:

```bash
export LDFLAGS="-L/opt/homebrew/opt/openssl@3/lib"
export CPPFLAGS="-I/opt/homebrew/opt/openssl@3/include"
export PATH="/opt/homebrew/opt/bison/bin:/opt/homebrew/opt/flex/bin:$PATH"
```

---

# ✅ **2. Configure**

From inside the repo directory:

```bash
./configure --prefix=$HOME/pg_build/caching-postgres/
```

If you want SSL:

```bash
./configure --enable-debug --prefix=$HOME/pg_build/caching-postgres/ --with-openssl
```

---

# ✅ **3. Build**

```bash
make -j$(sysctl -n hw.ncpu)
```

---

# ✅ **4. Install**

```bash
make install
```

Now your local PostgreSQL binaries are inside:

```
~/pg_build/caching-postgres/bin/
```

---

# ⭐ **5. Initialize a database cluster**

Pick some directory for data, e.g.:

```bash
mkdir ~/pgdata
~/pg_build/caching-postgres/bin/initdb -D ~/pgdata
```

---

# 🚀 **6. Start the PostgreSQL server**

```bash
~/pg_build/bin/pg_ctl -D ~/pgdata -l logfile start
```

This starts the server and writes logs to `logfile`.

---

# 🧠 **7. Connect using psql**

If you already have `psql` installed globally (Homebrew), it works fine:

```bash
psql -h localhost -p 5432
```

Or use your newly built one:

```bash
~/pg_build/bin/psql -h localhost
```

---

# 🔥 **8. Stop the server**

```bash
~/pg_build/bin/pg_ctl -D ~/pgdata stop
```

---

# ⚠️ Common Issues

### **❌ configure: error: flex is too old**

Install the Homebrew one and update your PATH:

```bash
brew install flex bison
export PATH="/opt/homebrew/opt/flex/bin:/opt/homebrew/opt/bison/bin:$PATH"
```

---

### **❌ "library not loaded: libssl.dylib"**

Set OpenSSL path:

```bash
export LDFLAGS="-L/opt/homebrew/opt/openssl@3/lib"
export CPPFLAGS="-I/opt/homebrew/opt/openssl@3/include"
```

---

If you want, I can also guide you on:

✅ Running PostgreSQL from source in **debug mode**
✅ Adding print logs / debugging executor nodes
✅ Editing PostgreSQL source and rebuilding quickly (`make -C src/backend`)
✅ Setting breakpoints with **lldb**

Just tell me!
