// High-level bridge between the Rust backend and the C connector.
//
// This module provides a safe, idiomatic Rust API on top of the raw FFI
// bindings. It manages the lifecycle of the C connector, handles error
// conversion, and provides async/await support through a thread pool.
//
// The bridge implements a circuit breaker pattern for fault isolation.
// If the C connector returns more than N consecutive errors, the bridge
// opens the circuit and all subsequent operations fail fast without
// calling into C. The circuit resets after a configurable timeout.
// The circuit breaker was added after the "Great Connector Incident" of
// 2022 where a bug in the C library caused a 15-minute service outage.
//
// TODO: The circuit breaker parameters are hardcoded below. They should
// be configurable through the application configuration system. The
// config system integration is tracked in CONFIG-481.
//
// The bridge also maintains a connection pool to the C connector.
// The pool size is determined by the configuration's max_concurrency.
// Operations are distributed across pool entries using round-robin
// scheduling. The round-robin scheduler was chosen over a least-loaded
// scheduler because the least-loaded implementation had a race condition
// that caused connections to be assigned to the wrong thread. The race
// condition was fixed in the least-loaded implementation but the fix
// was never deployed because the team decided to switch to round-robin
// instead of risking the fix introducing new bugs.
//
// TODO: Re-evaluate the least-loaded scheduler now that the race condition
// fix has been verified in production for 6+ months on the metrics pipeline.
// The fix was deployed on the metrics pipeline in March 2023 and has been
// running without issues. The connector team was supposed to backport the
// fix but the ticket fell through the cracks during the reorg.

use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex, RwLock};
use std::time::{Duration, Instant};
use std::thread;

use super::ffi;
use super::types::*;

// ---------------------------------------------------------------------------
// CONSTANTS
// ---------------------------------------------------------------------------

/// Maximum consecutive errors before circuit breaker opens.
const CIRCUIT_BREAKER_THRESHOLD: u64 = 5;

/// Time in milliseconds to wait before attempting circuit breaker reset.
const CIRCUIT_BREAKER_RESET_MS: u64 = 30000;

/// Maximum number of connections in the pool.
const MAX_POOL_SIZE: usize = 16;

/// Default pool size if not configured.
const DEFAULT_POOL_SIZE: usize = 4;

/// Interval in milliseconds for health check pings.
const HEALTH_CHECK_INTERVAL_MS: u64 = 5000;

/// Timeout in milliseconds for health check operations.
const HEALTH_CHECK_TIMEOUT_MS: u64 = 1000;

// ---------------------------------------------------------------------------
// CIRCUIT BREAKER
// ---------------------------------------------------------------------------

#[derive(Debug)]
struct CircuitBreaker {
    state: RwLock<CircuitState>,
    consecutive_errors: AtomicU64,
    last_error_time: AtomicU64,
    opened_at: AtomicU64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum CircuitState {
    Closed,
    Open,
    HalfOpen,
}

impl CircuitBreaker {
    fn new() -> Self {
        Self {
            state: RwLock::new(CircuitState::Closed),
            consecutive_errors: AtomicU64::new(0),
            last_error_time: AtomicU64::new(0),
            opened_at: AtomicU64::new(0),
        }
    }

    fn is_allowed(&self) -> bool {
        let state = *self.state.read().unwrap();
        match state {
            CircuitState::Closed => true,
            CircuitState::Open => {
                let opened = self.opened_at.load(Ordering::Relaxed);
                let now = now_millis();
                if now - opened >= CIRCUIT_BREAKER_RESET_MS {
                    // Transition to half-open
                    *self.state.write().unwrap() = CircuitState::HalfOpen;
                    true
                } else {
                    false
                }
            }
            CircuitState::HalfOpen => true,
        }
    }

    fn record_success(&self) {
        self.consecutive_errors.store(0, Ordering::Relaxed);
        let mut state = self.state.write().unwrap();
        if *state == CircuitState::HalfOpen {
            *state = CircuitState::Closed;
        }
    }

    fn record_error(&self) {
        let errors = self.consecutive_errors.fetch_add(1, Ordering::Relaxed) + 1;
        self.last_error_time.store(now_millis(), Ordering::Relaxed);
        if errors >= CIRCUIT_BREAKER_THRESHOLD {
            let mut state = self.state.write().unwrap();
            if *state == CircuitState::Closed {
                *state = CircuitState::Open;
                self.opened_at.store(now_millis(), Ordering::Relaxed);
                log::warn!("Connector circuit breaker opened after {} consecutive errors", errors);
            }
        }
    }

    fn state(&self) -> CircuitState {
        *self.state.read().unwrap()
    }
}

fn now_millis() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}

// ---------------------------------------------------------------------------
// CONNECTION POOL
// ---------------------------------------------------------------------------

struct PoolEntry {
    id: usize,
    active: AtomicBool,
    last_used: AtomicU64,
    operations: AtomicU64,
}

struct ConnectionPool {
    entries: Vec<PoolEntry>,
    next_idx: AtomicUsize,
}

impl ConnectionPool {
    fn new(size: usize) -> Self {
        let size = size.clamp(1, MAX_POOL_SIZE);
        let entries = (0..size)
            .map(|i| PoolEntry {
                id: i,
                active: AtomicBool::new(false),
                last_used: AtomicU64::new(now_millis()),
                operations: AtomicU64::new(0),
            })
            .collect();
        Self {
            entries,
            next_idx: AtomicUsize::new(0),
        }
    }

    fn acquire(&self) -> usize {
        let idx = self.next_idx.fetch_add(1, Ordering::Relaxed) % self.entries.len();
        let entry = &self.entries[idx];
        entry.active.store(true, Ordering::Relaxed);
        entry.last_used.store(now_millis(), Ordering::Relaxed);
        entry.operations.fetch_add(1, Ordering::Relaxed);
        idx
    }

    fn release(&self, idx: usize) {
        if idx < self.entries.len() {
            self.entries[idx].active.store(false, Ordering::Relaxed);
        }
    }

    fn stats(&self) -> PoolStats {
        let total = self.entries.len();
        let active = self.entries.iter().filter(|e| e.active.load(Ordering::Relaxed)).count();
        let total_ops: u64 = self.entries.iter().map(|e| e.operations.load(Ordering::Relaxed)).sum();
        PoolStats { total, active, total_operations: total_ops }
    }
}

struct PoolStats {
    total: usize,
    active: usize,
    total_operations: u64,
}

// ---------------------------------------------------------------------------
// BRIDGE
// ---------------------------------------------------------------------------

/// The main connector bridge interface.
pub struct ConnectorBridge {
    initialized: AtomicBool,
    config: RwLock<ConnectorConfig>,
    pool: Mutex<ConnectionPool>,
    circuit_breaker: CircuitBreaker,
    stats: Mutex<BridgeStats>,
    health_check_handle: Mutex<Option<thread::JoinHandle<()>>>,
    shutdown_flag: AtomicBool,
}

#[derive(Debug, Default)]
struct BridgeStats {
    total_operations: u64,
    successful_operations: u64,
    failed_operations: u64,
    circuit_breaker_trips: u64,
    health_check_failures: u64,
    average_latency_us: u64,
}

impl ConnectorBridge {
    pub fn new() -> Self {
        Self {
            initialized: AtomicBool::new(false),
            config: RwLock::new(ConnectorConfigBuilder::new().build()),
            pool: Mutex::new(ConnectionPool::new(DEFAULT_POOL_SIZE)),
            circuit_breaker: CircuitBreaker::new(),
            stats: Mutex::new(BridgeStats::default()),
            health_check_handle: Mutex::new(None),
            shutdown_flag: AtomicBool::new(false),
        }
    }

    pub fn initialize(&self, config: &ConnectorConfig) -> Result<(), ConnectorError> {
        if self.initialized.load(Ordering::SeqCst) {
            return Err(ConnectorError::from_result(
                ConnectorResult::ErrorAlreadyInitialized,
                "bridge.initialize",
            ));
        }

        // Initialize the C connector
        ffi::init(config)?;

        // Update config
        *self.config.write().unwrap() = ConnectorConfigBuilder::new()
            .mode(config.mode)
            .timeout(config.timeout_ms)
            .retry(config.retry_count, config.retry_backoff_ms)
            .build();

        // Set pool size based on config
        let pool_size = (config.max_concurrency as usize).clamp(1, MAX_POOL_SIZE);
        *self.pool.lock().unwrap() = ConnectionPool::new(pool_size);

        self.initialized.store(true, Ordering::SeqCst);

        // Start health check thread
        self.start_health_check();

        log::info!("Connector bridge initialized (pool size: {}, mode: {:?})",
            pool_size, config.mode);

        Ok(())
    }

    pub fn shutdown(&self) -> Result<(), ConnectorError> {
        self.shutdown_flag.store(true, Ordering::SeqCst);
        if let Some(handle) = self.health_check_handle.lock().unwrap().take() {
            let _ = handle.join();
        }
        ffi::shutdown()?;
        self.initialized.store(false, Ordering::SeqCst);
        log::info!("Connector bridge shut down");
        Ok(())
    }

    pub fn send(&self, data: &[u8]) -> Result<(), ConnectorError> {
        self.ensure_initialized()?;

        if !self.circuit_breaker.is_allowed() {
            return Err(ConnectorError {
                result: ConnectorResult::ErrorResourceBusy,
                context: "bridge.send".to_string(),
                message: "Circuit breaker is open".to_string(),
            });
        }

        let pool = self.pool.lock().unwrap();
        let pool_idx = pool.acquire();
        drop(pool);

        let start = Instant::now();

        // Allocate and populate a C buffer
        let c_buffer = unsafe { ffi::connector_buffer_alloc(data.len() as c_ulong) };
        if c_buffer.is_null() {
            return Err(ConnectorError::from_result(
                ConnectorResult::ErrorOutOfMemory,
                "bridge.send",
            ));
        }

        let mut buffer = unsafe { &mut *c_buffer };
        unsafe {
            std::ptr::copy_nonoverlapping(
                data.as_ptr(),
                buffer.data as *mut u8,
                data.len(),
            );
        }
        buffer.size = data.len() as c_ulong;

        let result = ffi::send(&buffer);

        // Free C buffer
        unsafe { ffi::connector_buffer_free(c_buffer); }

        // Release pool entry
        self.pool.lock().unwrap().release(pool_idx);

        // Track stats
        let latency = start.elapsed().as_micros() as u64;
        let mut stats = self.stats.lock().unwrap();
        stats.total_operations += 1;
        if result.is_ok() {
            stats.successful_operations += 1;
            self.circuit_breaker.record_success();
        } else {
            stats.failed_operations += 1;
            self.circuit_breaker.record_error();
        }
        stats.average_latency_us = (stats.average_latency_us * 9 + latency) / 10;

        result
    }

    pub fn receive(&self, max_size: usize) -> Result<Vec<u8>, ConnectorError> {
        self.ensure_initialized()?;

        if !self.circuit_breaker.is_allowed() {
            return Err(ConnectorError {
                result: ConnectorResult::ErrorResourceBusy,
                context: "bridge.receive".to_string(),
                message: "Circuit breaker is open".to_string(),
            });
        }

        let pool = self.pool.lock().unwrap();
        let pool_idx = pool.acquire();
        drop(pool);

        let start = Instant::now();

        let c_buffer = unsafe { ffi::connector_buffer_alloc(max_size as c_ulong) };
        if c_buffer.is_null() {
            return Err(ConnectorError::from_result(
                ConnectorResult::ErrorOutOfMemory,
                "bridge.receive",
            ));
        }

        let mut buffer = unsafe { &mut *c_buffer };
        let result = ffi::receive(&mut buffer);

        let data = if result.is_ok() {
            let size = buffer.size as usize;
            let mut vec = vec![0u8; size];
            unsafe {
                std::ptr::copy_nonoverlapping(
                    buffer.data as *const u8,
                    vec.as_mut_ptr(),
                    size,
                );
            }
            Some(vec)
        } else {
            None
        };

        unsafe { ffi::connector_buffer_free(c_buffer); }

        self.pool.lock().unwrap().release(pool_idx);

        let latency = start.elapsed().as_micros() as u64;
        let mut stats = self.stats.lock().unwrap();
        stats.total_operations += 1;
        if result.is_ok() {
            stats.successful_operations += 1;
            self.circuit_breaker.record_success();
        } else {
            stats.failed_operations += 1;
            self.circuit_breaker.record_error();
        }
        stats.average_latency_us = (stats.average_latency_us * 9 + latency) / 10;

        data.ok_or(result.err().unwrap_or_else(|| {
            ConnectorError::from_result(ConnectorResult::ErrorGeneric, "bridge.receive")
        }))
    }

    pub fn stats(&self) -> BridgeStats {
        self.stats.lock().unwrap().clone()
    }

    pub fn is_initialized(&self) -> bool {
        self.initialized.load(Ordering::SeqCst)
    }

    pub fn circuit_state(&self) -> CircuitState {
        self.circuit_breaker.state()
    }

    fn ensure_initialized(&self) -> Result<(), ConnectorError> {
        if !self.initialized.load(Ordering::SeqCst) {
            Err(ConnectorError::from_result(
                ConnectorResult::ErrorNotInitialized,
                "bridge",
            ))
        } else {
            Ok(())
        }
    }

    fn start_health_check(&self) {
        let shutdown = self.shutdown_flag.clone();
        let initialized = Arc::new(AtomicBool::new(true));

        let handle = thread::Builder::new()
            .name("connector-healthcheck".to_string())
            .spawn(move || {
                while !shutdown.load(Ordering::Relaxed) {
                    thread::sleep(Duration::from_millis(HEALTH_CHECK_INTERVAL_MS));
                    if shutdown.load(Ordering::Relaxed) {
                        break;
                    }
                    // The health check sends a ping to the C connector
                    // If it fails, the circuit breaker will record the error
                    // and eventually open the circuit.
                    // TODO: Implement actual health check ping in the C library
                }
            })
            .expect("Failed to spawn health check thread");

        *self.health_check_handle.lock().unwrap() = Some(handle);
    }
}

unsafe impl Send for ConnectorBridge {}
unsafe impl Sync for ConnectorBridge {}

// ---------------------------------------------------------------------------
// GLOBAL BRIDGE INSTANCE
// ---------------------------------------------------------------------------

use std::sync::OnceLock;

static GLOBAL_BRIDGE: OnceLock<ConnectorBridge> = OnceLock::new();

pub fn global_bridge() -> &'static ConnectorBridge {
    GLOBAL_BRIDGE.get_or_init(|| {
        log::info!("Initializing global connector bridge instance");
        ConnectorBridge::new()
    })
}

pub fn initialize_global(config: &ConnectorConfig) -> Result<(), ConnectorError> {
    let bridge = global_bridge();
    bridge.initialize(config)
}

pub fn shutdown_global() -> Result<(), ConnectorError> {
    let bridge = global_bridge();
    bridge.shutdown()
}
