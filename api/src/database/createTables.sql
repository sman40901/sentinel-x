-- Sentinel-X PostgreSQL schema
-- Initial V1 database design

-- =========================================================
-- 1. Devices
-- =========================================================

CREATE TABLE devices (
    device_id VARCHAR(100) PRIMARY KEY,
    group_id VARCHAR(50) NOT NULL,
    status VARCHAR(20) NOT NULL DEFAULT 'offline',
    last_seen TIMESTAMPTZ,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT chk_device_status
        CHECK (status IN ('online', 'offline'))
);

-- =========================================================
-- 2. Telemetry
-- =========================================================

CREATE TABLE telemetry (
    id BIGSERIAL PRIMARY KEY,
    device_id VARCHAR(100) NOT NULL,
    temperature DOUBLE PRECISION NOT NULL,
    humidity DOUBLE PRECISION NOT NULL,
    gas INTEGER NOT NULL,
    motion_detected BOOLEAN NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT fk_telemetry_device
        FOREIGN KEY (device_id)
        REFERENCES devices(device_id)
        ON DELETE CASCADE
);

-- =========================================================
-- 3. Alerts
-- =========================================================

CREATE TABLE alerts (
    id BIGSERIAL PRIMARY KEY,
    device_id VARCHAR(100) NOT NULL,
    alert_type VARCHAR(50) NOT NULL,
    level VARCHAR(50) NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT fk_alert_device
        FOREIGN KEY (device_id)
        REFERENCES devices(device_id)
        ON DELETE CASCADE
);

-- =========================================================
-- Indexes for dashboard/history queries
-- =========================================================

CREATE INDEX idx_telemetry_device_created
    ON telemetry(device_id, created_at DESC);

CREATE INDEX idx_alerts_device_created
    ON alerts(device_id, created_at DESC);
