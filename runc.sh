#!/bin/bash
set -e  # Stop if any command fails

# Step 1: Go to PostgreSQL source directory
echo "Changing directory to PostgreSQL source..."
cd postgres

# Step 2: Build PostgreSQL using all cores
echo "Building PostgreSQL..."
make -j$(nproc)

# Step 3: Install PostgreSQL
echo "Installing PostgreSQL..."
make install

cd

# Step 4: Restart PostgreSQL instance on port 5433
echo "Restarting PostgreSQL instance..."
/home/samarth/pgsql-dev/bin/pg_ctl -D /home/samarth/pgdataA -o "-p 5433" -l logA restart

# Step 5: Connect to the database using psql
echo "Connecting to PostgreSQL on port 5433..."
psql -p 5433 -d postgres
