use std::process::ExitCode;

use sturdy::prelude::*;
use sturdy::reflection;
use sturdy::sync::{mpsc, Mutex};
use sturdy::task;
use sturdy::render::{CursorIcon, SurfaceHandle, TextInputArea, WindowEffectKind};
use sturdy::{ScheduleTarget, R, W};

use std::sync::Arc;
use sturdy::sync::RwLock;
use sturdy::task::JoinSet;
use sturdy::time::{self, Duration, Instant};

/// Exercises `sturdy::task`/`sturdy::sync` end to end: a handful of spawned tasks (trivial work,
/// cooperative yielding, a panic, an abort), an mpsc channel between two tasks, and a shared
/// counter behind an async `Mutex` incremented concurrently.
///
/// Called from `GameLogic::init` (see below), where the engine's scheduler is guaranteed to
/// already be running (`sturdy::task::spawn` submits work to it) — it is not backed by its own
/// thread pool. Driven with `sturdy::task::block_on`, which just parks the calling thread (the
/// engine's init-callback thread here) between wakeups while the scheduler's worker threads do the
/// actual polling.
fn demo_async_tasks() {
    task::block_on(async {
        // -- Trivial CPU work -------------------------------------------------------------
        let trivial = task::spawn(async { 2 + 2 });

        // -- Cooperative yielding: yield_now() a couple of times, proving Pending-then-Ready --
        let yielding = task::spawn(async {
            let mut steps = 0;
            task::yield_now().await;
            steps += 1;
            task::yield_now().await;
            steps += 1;
            steps
        });

        // -- A task that deliberately panics -----------------------------------------------
        //
        // Safe to catch here (see `task.rs`'s module doc): a spawned task's poll loop never
        // crosses into C++, unlike the game-logic/UI-draw/log-sink callbacks elsewhere in
        // `sturdy`/`sturdy-sys`, which each guard their own C++ boundary individually with
        // `catch_unwind` + an explicit `abort()` regardless of this workspace's panic strategy
        // (see the `[profile.release]` comment in the workspace `Cargo.toml`).
        let panicking = task::spawn(async {
            #[allow(unreachable_code)]
            if true {
                panic!("boom: deliberate panic to prove JoinError::is_panic()");
            }
            0
        });

        // -- A task aborted before it can complete -------------------------------------------
        let abort_target = task::spawn(async {
            loop {
                task::yield_now().await;
            }
        });
        abort_target.abort();

        eprintln!("[task-demo] trivial = {:?}", trivial.await);

        match yielding.await {
            Ok(steps) => eprintln!("[task-demo] yielding task completed after {steps} yields"),
            Err(e) => eprintln!("[task-demo] yielding task failed unexpectedly: {e}"),
        }

        match panicking.await {
            Ok(_) => eprintln!("[task-demo] panicking task unexpectedly succeeded"),
            Err(e) => {
                eprintln!(
                    "[task-demo] panicking task -> JoinError: is_panic={} is_cancelled={} ({e})",
                    e.is_panic(),
                    e.is_cancelled()
                );
            }
        }

        match abort_target.await {
            Ok(_) => eprintln!("[task-demo] aborted task unexpectedly succeeded"),
            Err(e) => {
                eprintln!(
                    "[task-demo] aborted task -> JoinError: is_panic={} is_cancelled={} ({e})",
                    e.is_panic(),
                    e.is_cancelled()
                );
            }
        }

        // -- mpsc: producer/consumer, confirm ordering ---------------------------------------
        let (tx, mut rx) = mpsc::channel::<i32>(2);
        let producer = task::spawn(async move {
            for value in 1..=5 {
                tx.send(value).await.expect("receiver still alive");
                task::yield_now().await;
            }
        });
        let consumer = task::spawn(async move {
            let mut received = Vec::new();
            while let Some(value) = rx.recv().await {
                received.push(value);
            }
            received
        });
        producer.await.expect("producer task panicked");
        let received = consumer.await.expect("consumer task panicked");
        eprintln!("[task-demo] mpsc received in order: {received:?} (expected [1, 2, 3, 4, 5])");
        assert_eq!(received, vec![1, 2, 3, 4, 5]);

        // -- Async Mutex: two tasks concurrently incrementing a shared counter ----------------
        //
        // No `yield_now()` in the hot loop here: the two tasks already run genuinely
        // concurrently (each spawned task is polled from the engine's own worker pool, so both
        // can be mid-`lock()` on different OS threads at once), which is exactly the real
        // contention this is meant to exercise. Adding a yield per iteration would only force an
        // extra full scheduler round-trip (spawn -> worker wakeup) per increment for no benefit
        // — `yield_now` is already proven separately above.
        const INCREMENTS_PER_TASK: u32 = 1000;
        let counter = std::sync::Arc::new(Mutex::new(0_u32));
        let mut handles = Vec::new();
        for _ in 0..2 {
            let counter = counter.clone();
            handles.push(task::spawn(async move {
                for _ in 0..INCREMENTS_PER_TASK {
                    let mut guard = counter.lock().await;
                    *guard += 1;
                }
            }));
        }
        for handle in handles {
            handle.await.expect("counter task panicked");
        }
        let final_count = *counter.lock().await;
        let expected = INCREMENTS_PER_TASK * 2;
        eprintln!("[task-demo] shared counter after concurrent increments: {final_count} (expected {expected})");
        assert_eq!(final_count, expected);

        eprintln!("[task-demo] all checks passed");
    });
}

/// Exercises the async-API extensions added on top of `sturdy::task`/`sturdy::sync`
/// (`select!` including `biased`/`if guard`/`else`, `JoinSet` including `detach_all`,
/// `sturdy::time::{sleep, timeout, interval}`, the unbounded mpsc channel, and `RwLock`), logged
/// with an `[async-ext]` prefix to keep it separate from `demo_async_tasks`'s own
/// `[task-demo]`-prefixed lines. Same driving pattern as `demo_async_tasks`: called from
/// `GameLogic::init` and driven with `task::block_on`.
fn demo_async_ext() {
    task::block_on(async {
        // -- select!: race two tasks, confirm exactly one side effect happened ----------------
        //
        // `fast` finishes almost immediately (a couple of `yield_now`s); `slow` sleeps for a full
        // second. `select!` must run `fast`'s arm and drop (cancel) `slow`'s branch before it
        // ever gets to increment `slow_ran`, proving the loser's side effect never happened.
        let fast_ran = Arc::new(Mutex::new(false));
        let slow_ran = Arc::new(Mutex::new(false));
        {
            let fast_ran = fast_ran.clone();
            let slow_ran = slow_ran.clone();
            let fast = async {
                task::yield_now().await;
                task::yield_now().await;
                *fast_ran.lock().await = true;
                "fast"
            };
            let slow = async {
                time::sleep(Duration::from_secs(1)).await;
                *slow_ran.lock().await = true;
                "slow"
            };
            let winner = sturdy::select! {
                result = fast => { result }
                result = slow => { result }
            };
            eprintln!("[async-ext] select! winner = {winner:?}");
            assert_eq!(winner, "fast");
        }
        let fast_ran = *fast_ran.lock().await;
        let slow_ran = *slow_ran.lock().await;
        eprintln!("[async-ext] select! side effects: fast_ran={fast_ran} slow_ran={slow_ran} (expected true, false)");
        assert!(fast_ran, "the winning branch's side effect must have happened");
        assert!(!slow_ran, "the losing branch must have been cancelled before its side effect ran");

        // -- select!: `biased;`, a disabled `if guard` branch, and an `else` arm ---------------
        let biased_winner = sturdy::select! {
            biased;
            v = async { 1 } => { v }
            v = async { 2 } => { v }
        };
        eprintln!("[async-ext] select! biased winner = {biased_winner} (expected 1, first-listed wins a tie)");
        assert_eq!(biased_winner, 1);

        let guard_winner = sturdy::select! {
            v = async { "enabled" }, if true => { v }
            v = async { "disabled" }, if false => { v }
        };
        eprintln!("[async-ext] select! guard winner = {guard_winner} (expected \"enabled\")");
        assert_eq!(guard_winner, "enabled");

        let else_winner = sturdy::select! {
            _v = std::future::pending::<()>(), if false => { "never" }
            else => { "else" }
        };
        eprintln!("[async-ext] select! all-disabled -> else = {else_winner} (expected \"else\")");
        assert_eq!(else_winner, "else");

        // -- JoinSet: several tasks (including one that panics), drained in completion order ---
        let mut set: JoinSet<u32> = JoinSet::new();
        set.spawn(async {
            task::yield_now().await;
            task::yield_now().await;
            task::yield_now().await;
            1
        });
        set.spawn(async { 2 });
        set.spawn(async {
            task::yield_now().await;
            panic!("boom: deliberate JoinSet panic to prove join_next surfaces it as Err");
        });
        set.spawn(async {
            task::yield_now().await;
            4
        });
        assert_eq!(set.len(), 4);

        let mut ok_sum = 0_u32;
        let mut ok_count = 0;
        let mut panic_count = 0;
        while let Some(result) = set.join_next().await {
            match result {
                Ok(value) => {
                    eprintln!("[async-ext] JoinSet task completed: {value}");
                    ok_sum += value;
                    ok_count += 1;
                }
                Err(e) => {
                    eprintln!("[async-ext] JoinSet task failed: is_panic={} ({e})", e.is_panic());
                    assert!(e.is_panic());
                    panic_count += 1;
                }
            }
        }
        assert!(set.is_empty());
        eprintln!(
            "[async-ext] JoinSet drained: {ok_count} ok (sum={ok_sum}), {panic_count} panicked (expected 3 ok summing to 7, 1 panicked)"
        );
        assert_eq!(ok_count, 3);
        assert_eq!(ok_sum, 7);
        assert_eq!(panic_count, 1);

        // -- JoinSet::detach_all: forgotten tasks keep running but stop being tracked ----------
        let mut detached: JoinSet<()> = JoinSet::new();
        for _ in 0..3 {
            detached.spawn(async {
                task::yield_now().await;
            });
        }
        assert_eq!(detached.len(), 3);
        detached.detach_all();
        eprintln!("[async-ext] JoinSet::detach_all: len after detach = {} (expected 0)", detached.len());
        assert_eq!(detached.len(), 0);
        assert!(detached.is_empty());
        assert!(detached.join_next().await.is_none(), "a detached set has nothing left to join");

        // -- time::interval: first tick fires immediately, later ticks land on schedule -------
        let period = Duration::from_millis(50);
        let mut ticker = time::interval(period);
        let before = Instant::now();
        ticker.tick().await;
        let first_tick_elapsed = before.elapsed();
        eprintln!("[async-ext] interval first tick elapsed = {first_tick_elapsed:?} (expected ~immediate)");
        assert!(first_tick_elapsed < period, "the first interval tick must not wait a full period");
        ticker.tick().await;
        let two_ticks_elapsed = before.elapsed();
        eprintln!("[async-ext] interval second tick elapsed since start = {two_ticks_elapsed:?} (expected >= {period:?})");
        assert!(two_ticks_elapsed >= period, "the second interval tick must land roughly one period after the first");

        // -- time::sleep: prove real time actually elapsed -------------------------------------
        let sleep_for = Duration::from_millis(200);
        let before = Instant::now();
        time::sleep(sleep_for).await;
        let measured = before.elapsed();
        eprintln!("[async-ext] sleep({sleep_for:?}) measured elapsed = {measured:?}");
        assert!(measured >= sleep_for, "sleep must not return before its deadline");

        // -- time::timeout: a future that never finishes, racing a timer that does ------------
        let before = Instant::now();
        let result = time::timeout(Duration::from_millis(150), std::future::pending::<()>()).await;
        let measured = before.elapsed();
        eprintln!("[async-ext] timeout(150ms, pending()) -> {:?}, measured elapsed = {measured:?}", result.is_err());
        assert!(result.is_err(), "timeout must elapse against a future that never completes");
        assert!(measured >= Duration::from_millis(150));

        // A timeout racing a future that finishes comfortably in time must come back `Ok`.
        let quick = time::timeout(Duration::from_secs(1), async {
            task::yield_now().await;
            42
        })
        .await;
        eprintln!("[async-ext] timeout(1s, quick task) -> {quick:?}");
        assert_eq!(quick.ok(), Some(42));

        // -- unbounded mpsc: producer/consumer, confirm ordering, send never pends ------------
        let (tx, mut rx) = mpsc::unbounded_channel::<i32>();
        // Unbounded `send` is synchronous: fire off every value before the consumer even starts,
        // something the bounded channel (capacity 2) in `demo_async_tasks` cannot do.
        for value in 1..=5 {
            tx.send(value).expect("receiver still alive");
        }
        drop(tx);
        let consumer = task::spawn(async move {
            let mut received = Vec::new();
            while let Some(value) = rx.recv().await {
                received.push(value);
            }
            received
        });
        let received = consumer.await.expect("unbounded consumer task panicked");
        eprintln!("[async-ext] unbounded mpsc received in order: {received:?} (expected [1, 2, 3, 4, 5])");
        assert_eq!(received, vec![1, 2, 3, 4, 5]);

        // -- RwLock: concurrent readers + a writer, readers see consistent data ---------------
        let rw = Arc::new(RwLock::new((0_i64, 0_i64))); // invariant: .0 == .1 always
        let mut readers = JoinSet::new();
        for _ in 0..4 {
            let rw = rw.clone();
            readers.spawn(async move {
                let mut consistent = true;
                for _ in 0..50 {
                    let guard = rw.read().await;
                    if guard.0 != guard.1 {
                        consistent = false;
                    }
                    drop(guard);
                    task::yield_now().await;
                }
                consistent
            });
        }
        let writer = {
            let rw = rw.clone();
            task::spawn(async move {
                for i in 1..=20_i64 {
                    let mut guard = rw.write().await;
                    // Briefly break the invariant mid-write, on purpose: if a reader ever
                    // observed this, `consistent` above would go false, since `.0` and `.1` are
                    // set in two separate statements while holding the exclusive write guard.
                    guard.0 = i;
                    task::yield_now().await;
                    guard.1 = i;
                    drop(guard);
                    task::yield_now().await;
                }
            })
        };
        writer.await.expect("RwLock writer task panicked");
        let mut all_consistent = true;
        while let Some(result) = readers.join_next().await {
            all_consistent &= result.expect("RwLock reader task panicked");
        }
        let final_value = *rw.read().await;
        eprintln!(
            "[async-ext] RwLock: readers always saw a consistent pair = {all_consistent}, final value visible to a reader after the writer = {final_value:?} (expected true, (20, 20))"
        );
        assert!(all_consistent, "no reader should ever observe a torn write");
        assert_eq!(final_value, (20, 20));

        eprintln!("[async-ext] all checks passed");
    });
}

/// Exercises the RHI extensions added on top of the base bridge (buffer mapping, fill/update
/// buffer, blit, texture clears, query sets, debug groups, indirect dispatch) against the real
/// `RhiDevice`. Logged with an `[rhi-ext]` prefix. Must run once the renderer is actually up (a
/// real frame callback, not `init`), since `Engine::rhi` needs a live `RhiDevice` — see
/// `Engine::gpu`'s doc comment ("once the renderer is initialized").
fn demonstrate_rhi_ext(engine: &mut Engine<'_>) {
    use sturdy::rhi::{
        BufferDesc, BufferUsage, ClearColor, ClearDepthStencilValue, CommandEncoderDesc, Format,
        HandleExt, MemoryLocation, PipelineStage, QueryResultFlags, QuerySetDesc, QueryType,
        QueueClass, SampleCount, TextureDesc, TextureDimension, TextureSubresourceRange,
        TextureUsage,
    };

    let mut rhi = engine.rhi();

    // -- Device introspection: limits, features, extensions, queues ------------------------------
    {
        let limits = rhi.limits();
        let features = rhi.enabled_features();
        let all_features = sturdy::rhi::all_feature_names();
        let extensions = rhi.enabled_extensions();
        let queues = rhi.queue_infos();
        eprintln!(
            "[rhi-ext] limits: max_tex2d={} max_bind_groups={} max_push={} workgroup=({},{},{}) ubo_align={} ts_period={}ns",
            limits.max_texture_dimension_2d, limits.max_bind_groups, limits.max_push_constants_size,
            limits.max_compute_workgroup_size_x, limits.max_compute_workgroup_size_y, limits.max_compute_workgroup_size_z,
            limits.min_uniform_buffer_offset_alignment, limits.timestamp_period_ns
        );
        eprintln!(
            "[rhi-ext] features: {} of {} enabled (first: {:?}); extensions: {} (first: {:?}); queues: {:?}",
            features.len(),
            all_features.len(),
            features.first(),
            extensions.len(),
            extensions.first(),
            queues.iter().map(|q| (q.queue, q.capabilities, q.lane_count)).collect::<Vec<_>>()
        );
        assert!(limits.max_texture_dimension_2d >= 4096 && limits.max_bind_groups >= 4);
        assert!(limits.min_uniform_buffer_offset_alignment.is_power_of_two());
        assert!(!all_features.is_empty() && features.len() <= all_features.len());
        assert!(features.iter().all(|f| all_features.contains(f)));
        assert!(queues.iter().any(|q| q.queue == QueueClass::Graphics && q.capabilities & 1 != 0));
    }

    // -- Feature/device properties: cheap, read-only, no pipeline/resource needed ----------------
    let properties = rhi.feature_properties();
    eprintln!(
        "[rhi-ext] feature_properties: ray_tracing.max_ray_recursion_depth={} \
         ray_tracing.shader_group_handle_size={} mesh_shader.max_mesh_output_vertices={} \
         subgroup.min_subgroup_size={} (0 in any field means that feature is unsupported on this device)",
        properties.ray_tracing.max_ray_recursion_depth,
        properties.ray_tracing.shader_group_handle_size,
        properties.mesh_shader.max_mesh_output_vertices,
        properties.subgroup.min_subgroup_size,
    );

    // -- Buffer mapping: map a HostUpload buffer, write through the mapped slice, read it back --
    let buffer = rhi
        .create_buffer(&BufferDesc {
            size: 256,
            usage: (BufferUsage::TRANSFER_SRC | BufferUsage::TRANSFER_DST).bits(),
            memory: MemoryLocation::HostUpload,
            label: "rhi-ext-mapped-buffer".to_string(),
        })
        .expect("create mapped buffer");
    assert!(buffer.is_valid());

    {
        let mut mapped = rhi.map_buffer(buffer).expect("map buffer");
        for (i, byte) in mapped.iter_mut().enumerate() {
            *byte = (i % 256) as u8;
        }
        // `mapped` drops here, unmapping the buffer.
    }
    {
        let mapped = rhi.map_buffer(buffer).expect("re-map buffer to read back");
        let ok = mapped.iter().enumerate().all(|(i, &b)| b == (i % 256) as u8);
        eprintln!("[rhi-ext] map/write/unmap/remap round trip: {} (expected true)", ok);
        assert!(ok, "buffer contents must survive an unmap/remap cycle");
    }

    // -- fill_buffer / update_buffer, verified via a HostReadback buffer + copy_buffer_to_buffer --
    let readback = rhi
        .create_buffer(&BufferDesc {
            size: 256,
            usage: BufferUsage::TRANSFER_DST.bits(),
            memory: MemoryLocation::HostReadback,
            label: "rhi-ext-readback-buffer".to_string(),
        })
        .expect("create readback buffer");

    let mut encoder = rhi
        .create_command_encoder(&CommandEncoderDesc { queue: QueueClass::Graphics, label: "rhi-ext-encoder".to_string() })
        .expect("create command encoder");

    // Debug-group-wrapped work: fill_buffer with a known 32-bit pattern, then update_buffer to
    // overwrite the first 8 bytes, all inside a push/pop debug group pair (visible in RenderDoc/
    // Nsight captures; here it just proves the calls don't crash and the work still executes).
    encoder.push_debug_group("rhi-ext: fill+update+clear");
    encoder.fill_buffer(buffer, 0, 256, 0xAABBCCDDu32.to_le());
    let update_bytes: [u8; 8] = [1, 2, 3, 4, 5, 6, 7, 8];
    encoder.update_buffer(buffer, 0, &update_bytes);

    // -- Texture clear: create a small color texture, clear it, and copy a texel out via a buffer
    let texture = rhi
        .create_texture(&TextureDesc {
            dimension: TextureDimension::Dim2D,
            format: Format::RGBA8Unorm,
            width: 4,
            height: 4,
            depth_or_layers: 1,
            mip_levels: 1,
            samples: SampleCount::X1,
            usage: (TextureUsage::TRANSFER_SRC | TextureUsage::TRANSFER_DST | TextureUsage::COLOR_ATTACHMENT).bits(),
            label: "rhi-ext-clear-texture".to_string(),
        })
        .expect("create clear texture");

    encoder.barrier(
        &[],
        &[],
        &[sturdy::rhi::TextureBarrier {
            texture,
            src_stage: PipelineStage::NONE.bits(),
            src_access: 0,
            dst_stage: PipelineStage::TRANSFER.bits(),
            dst_access: sturdy::rhi::AccessFlags::TRANSFER_WRITE.bits(),
            ownership: Default::default(),
            old_layout: sturdy::rhi::TextureLayout::Undefined,
            new_layout: sturdy::rhi::TextureLayout::TransferDst,
            range: TextureSubresourceRange { base_mip_level: 0, mip_level_count: 1, base_array_layer: 0, array_layer_count: 1 },
        }],
    );
    encoder.clear_color_texture(
        texture,
        &ClearColor { r: 0.25, g: 0.5, b: 0.75, a: 1.0 },
        &TextureSubresourceRange { base_mip_level: 0, mip_level_count: 1, base_array_layer: 0, array_layer_count: 1 },
    );
    // Also exercise clear_depth_stencil_texture's argument plumbing on a throwaway depth texture.
    let depth_texture = rhi
        .create_texture(&TextureDesc {
            dimension: TextureDimension::Dim2D,
            format: Format::D32Float,
            width: 4,
            height: 4,
            depth_or_layers: 1,
            mip_levels: 1,
            samples: SampleCount::X1,
            usage: TextureUsage::DEPTH_STENCIL_ATTACHMENT.bits(),
            label: "rhi-ext-depth-texture".to_string(),
        })
        .expect("create depth texture");
    encoder.barrier(
        &[],
        &[],
        &[sturdy::rhi::TextureBarrier {
            texture: depth_texture,
            src_stage: PipelineStage::NONE.bits(),
            src_access: 0,
            dst_stage: PipelineStage::EARLY_FRAGMENT_TESTS.bits(),
            dst_access: sturdy::rhi::AccessFlags::DEPTH_STENCIL_ATTACHMENT_WRITE.bits(),
            ownership: Default::default(),
            old_layout: sturdy::rhi::TextureLayout::Undefined,
            new_layout: sturdy::rhi::TextureLayout::DepthStencilAttachment,
            range: TextureSubresourceRange { base_mip_level: 0, mip_level_count: 1, base_array_layer: 0, array_layer_count: 1 },
        }],
    );
    encoder.clear_depth_stencil_texture(
        depth_texture,
        &ClearDepthStencilValue { depth: 0.0, stencil: 0 },
        &TextureSubresourceRange { base_mip_level: 0, mip_level_count: 1, base_array_layer: 0, array_layer_count: 1 },
    );
    encoder.pop_debug_group();

    // Barrier the clear-target texture to TransferSrc, then copy its top-left texel into the
    // readback buffer so the clear color can be verified on the CPU.
    encoder.barrier(
        &[],
        &[],
        &[sturdy::rhi::TextureBarrier {
            texture,
            src_stage: PipelineStage::TRANSFER.bits(),
            src_access: sturdy::rhi::AccessFlags::TRANSFER_WRITE.bits(),
            dst_stage: PipelineStage::TRANSFER.bits(),
            dst_access: sturdy::rhi::AccessFlags::TRANSFER_READ.bits(),
            ownership: Default::default(),
            old_layout: sturdy::rhi::TextureLayout::TransferDst,
            new_layout: sturdy::rhi::TextureLayout::TransferSrc,
            range: TextureSubresourceRange { base_mip_level: 0, mip_level_count: 1, base_array_layer: 0, array_layer_count: 1 },
        }],
    );
    encoder.copy_texture_to_buffer(
        texture,
        readback,
        &sturdy::rhi::BufferTextureCopy {
            buffer_offset: 0,
            buffer_row_length: 4,
            buffer_image_height: 4,
            mip_level: 0,
            base_array_layer: 0,
            array_layer_count: 1,
            offset_x: 0,
            offset_y: 0,
            offset_z: 0,
            extent_width: 4,
            extent_height: 4,
            extent_depth_or_layers: 1,
        },
    );

    // copy_texture_to_texture: copy the cleared texture's bottom-right 2x2 into a second texture,
    // then read one texel of it back into the readback buffer at offset 128.
    let copy_dst = rhi
        .create_texture(&TextureDesc {
            dimension: TextureDimension::Dim2D,
            format: Format::RGBA8Unorm,
            width: 2,
            height: 2,
            depth_or_layers: 1,
            mip_levels: 1,
            samples: SampleCount::X1,
            usage: (TextureUsage::TRANSFER_SRC | TextureUsage::TRANSFER_DST).bits(),
            label: "rhi-ext-copy-dst".to_string(),
        })
        .expect("create copy destination texture");
    let one_mip = TextureSubresourceRange { base_mip_level: 0, mip_level_count: 1, base_array_layer: 0, array_layer_count: 1 };
    encoder.barrier(
        &[],
        &[],
        &[sturdy::rhi::TextureBarrier {
            texture: copy_dst,
            src_stage: PipelineStage::NONE.bits(),
            src_access: 0,
            dst_stage: PipelineStage::TRANSFER.bits(),
            dst_access: sturdy::rhi::AccessFlags::TRANSFER_WRITE.bits(),
            ownership: Default::default(),
            old_layout: sturdy::rhi::TextureLayout::Undefined,
            new_layout: sturdy::rhi::TextureLayout::TransferDst,
            range: one_mip,
        }],
    );
    let layers = sturdy::rhi::TextureSubresourceLayers { mip_level: 0, base_array_layer: 0, array_layer_count: 1 };
    encoder.copy_texture_to_texture(
        texture,
        copy_dst,
        &sturdy::rhi::TextureCopy {
            src_subresource: layers,
            src_x: 2,
            src_y: 2,
            src_z: 0,
            dst_subresource: layers,
            dst_x: 0,
            dst_y: 0,
            dst_z: 0,
            width: 2,
            height: 2,
            depth: 1,
        },
    );
    encoder.barrier(
        &[],
        &[],
        &[sturdy::rhi::TextureBarrier {
            texture: copy_dst,
            src_stage: PipelineStage::TRANSFER.bits(),
            src_access: sturdy::rhi::AccessFlags::TRANSFER_WRITE.bits(),
            dst_stage: PipelineStage::TRANSFER.bits(),
            dst_access: sturdy::rhi::AccessFlags::TRANSFER_READ.bits(),
            ownership: Default::default(),
            old_layout: sturdy::rhi::TextureLayout::TransferDst,
            new_layout: sturdy::rhi::TextureLayout::TransferSrc,
            range: one_mip,
        }],
    );
    encoder.copy_texture_to_buffer(
        copy_dst,
        readback,
        &sturdy::rhi::BufferTextureCopy {
            buffer_offset: 128,
            buffer_row_length: 2,
            buffer_image_height: 2,
            mip_level: 0,
            base_array_layer: 0,
            array_layer_count: 1,
            offset_x: 0,
            offset_y: 0,
            offset_z: 0,
            extent_width: 2,
            extent_height: 2,
            extent_depth_or_layers: 1,
        },
    );

    // Copy the fill/update-buffer target's first 32 bytes into the second half of the readback
    // buffer so both checks share one submission.
    encoder.copy_buffer_to_buffer(buffer, readback, &sturdy::rhi::BufferCopy { src_offset: 0, dst_offset: 64, size: 32 });

    // -- Query set round trip: a timestamp query written mid-encoder, resolved into `readback` --
    let query_set = rhi
        .create_query_set(&QuerySetDesc { query_type: QueryType::Timestamp, count: 1, statistics: 0, label: "rhi-ext-timestamps".to_string() })
        .expect("create timestamp query set");
    encoder.reset_query_set(query_set, 0, 1);
    encoder.write_timestamp(PipelineStage::TRANSFER, query_set, 0);

    let command_buffer = encoder.finish().expect("finish rhi-ext command buffer");
    rhi.submit(&[command_buffer]).expect("submit rhi-ext command buffer");
    // `submit` does not itself block; wait for the GPU work to actually finish before touching
    // its results (the readback-buffer contents, the query result) from the CPU.
    rhi.wait_idle();

    {
        let mapped = rhi.map_buffer(readback).expect("map readback buffer");
        let texel = &mapped[0..4];
        eprintln!(
            "[rhi-ext] clear_color_texture readback texel (RGBA8) = {:?} (expected close to [64, 128, 191, 255])",
            texel
        );
        let update_readback = &mapped[64..72];
        eprintln!(
            "[rhi-ext] fill_buffer+update_buffer readback bytes[0..8] = {:?} (expected {:?})",
            update_readback, update_bytes
        );
        assert_eq!(update_readback, &update_bytes[..], "update_buffer must have overwritten fill_buffer's pattern");
        assert_eq!(&mapped[128..132], texel, "copy_texture_to_texture carried the cleared color over");
        assert_eq!(&mapped[128 + 12..128 + 16], texel, "all four copied texels match the source");
    }

    let mut timestamp_bytes = [0u8; 8];
    match rhi.get_query_set_results(query_set, 0, 1, &mut timestamp_bytes, 8, QueryResultFlags::RESULT_64_BIT | QueryResultFlags::WAIT) {
        Ok(()) => {
            let ts = u64::from_le_bytes(timestamp_bytes);
            eprintln!("[rhi-ext] query set round trip: timestamp = {ts} (expected nonzero)");
        }
        Err(e) => eprintln!("[rhi-ext] query set round trip: get_query_set_results failed: {e} (backend may not support timestamps)"),
    }

    rhi.destroy_query_set(query_set);
    rhi.destroy_texture(depth_texture);
    rhi.destroy_texture(copy_dst);
    rhi.destroy_texture(texture);
    rhi.destroy_buffer(readback);
    rhi.destroy_buffer(buffer);

    eprintln!("[rhi-ext] all RHI-extension demonstration checks passed");
}

/// Slang source for `demonstrate_render_pipeline_ext`: three flat-shaded triangles sharing one
/// vertex layout (`position: float2`, `color: float3`), selected by `first_vertex` at draw time --
/// left-red (vertices 0..3), right-green (3..6), and full-screen-white (6..9, used only by the
/// blend-constant pass). No `import`s (this binding's `compile_spirv` passes no search path), and
/// no resources (no uniforms/textures), so an empty `PipelineLayoutDesc` is valid for it.
const PIPELINE_EXT_SHADER_SOURCE: &str = r#"
struct VertexInput {
    float2 position : POSITION;
    float3 color : COLOR0;
};

struct VertexOutput {
    float4 position : SV_Position;
    float3 color : COLOR0;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input) {
    VertexOutput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.color = input.color;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    return float4(input.color, 1.0);
}
"#;

/// Exercises the RHI surface that actually needs a real render pipeline + render pass to reach:
/// [`sturdy::rhi::Rhi::feature_properties`] and the RHI-extension buffer/texture/query-set work in
/// `demonstrate_rhi_ext` never touch a `RenderPipeline` at all, so
/// `RenderPass::draw_indirect_multi`/`draw_indexed_indirect_multi`, `set_depth_bounds`,
/// `set_stencil_reference`, and `set_blend_constant` (plus the `DepthStencilState`/
/// `MultisampleState` pipeline fields backing depth-bounds/stencil/sample-locations) had never run
/// against a real draw before this. Compiles a real Slang shader to SPIR-V
/// ([`sturdy::shader_compiler`]) and renders into a small offscreen target, reading back individual
/// texels to check actual pixel output rather than just "didn't crash".
///
/// What each pass proves:
///  - Pass 1: `draw_indirect_multi` with `draw_count = 1` (always legal even without
///    `multiDrawIndirect`) draws the left/red triangle; `set_stencil_reference`/
///    `set_depth_bounds(0.0, 1.0)` are exercised alongside it (stencil is a smoke test only --
///    this binding has no stencil-aspect-specific texture readback yet to verify the write
///    landed, only that enabling it didn't break color output).
///  - Pass 2: the same `draw_indirect_multi` call with `draw_count = 2` draws *both* triangles
///    from one call -- the actual "multi" in multi-draw-indirect (assumes the GPU supports
///    `multiDrawIndirect`; every desktop GPU this is likely to run on does, but this binding has
///    no capability query for it to check first).
///  - Pass 3: `set_depth_bounds(0.0, 0.5)` against a depth attachment cleared to 1.0 -- the bounds
///    test compares against whatever is *currently stored* in the depth attachment, not the
///    incoming fragment's own depth, so this must reject the whole draw (leaving the clear color)
///    rather than silently ignoring the call.
///  - Pass 4: a second pipeline with blending enabled, `set_blend_constant` set to a known value,
///    blending a solid white triangle over a black clear with `ConstantColor`/
///    `OneMinusConstantColor` factors -- the resulting texel is an exact, predictable value, not
///    just "some color changed".
///
/// Logged with a `[pipeline-ext]` prefix. Must run once the renderer is up, same gating as
/// `demonstrate_rhi_ext`.
fn demonstrate_render_pipeline_ext(engine: &mut Engine<'_>) {
    use sturdy::rhi::{
        BlendComponent, BlendFactor, BlendOp, BufferDesc, BufferUsage, ClearColor,
        ColorAttachment, ColorTargetState, ColorWriteMask, CommandEncoderDesc,
        CompareOp, CullMode, DepthStencilAttachment, DepthStencilState, DrawArgs, Format, FrontFace,
        MemoryLocation, MultisampleState, PipelineLayoutDesc, PipelineStage, PolygonMode,
        PrimitiveTopology, QueueClass, RasterizationState, Rect2D, RenderPassDesc, RenderPipelineDesc,
        SampleCount, ShaderEntry, ShaderLanguage, ShaderModuleDesc, ShaderStage, StencilFaceState,
        StencilOp, StoreOp, TextureBarrier, TextureDesc, TextureDimension, TextureLayout,
        TextureSubresourceRange, TextureUsage, TextureViewDesc, TextureViewType, VertexAttribute,
        VertexBufferLayout, VertexFormat, VertexStepMode, Viewport,
    };
    use sturdy::rhi::AccessFlags;
    use sturdy::shader_compiler::{self, EntryPoint};

    const SIZE: u32 = 32;
    // Sampled at y = 0 (NDC), which maps to the middle row regardless of whichever way this
    // backend's viewport Y-convention actually flips -- avoids needing to know that convention to
    // predict a pixel coordinate.
    const MID_ROW: u32 = SIZE / 2;
    const LEFT_COL: u32 = SIZE / 4; // inside the red triangle at y=0 (spans x in [-0.9, -0.1])
    const RIGHT_COL: u32 = SIZE * 3 / 4; // inside the green triangle at y=0 (mirror of red)
    const CENTER_COL: u32 = SIZE / 2; // inside the full-screen white triangle

    // -- Compile the shared shader once -----------------------------------------------------------
    let mut modules = shader_compiler::compile_spirv(
        PIPELINE_EXT_SHADER_SOURCE,
        "hello_pipeline_ext",
        &[
            EntryPoint::new("vertexMain", shader_compiler::ShaderStage::Vertex),
            EntryPoint::new("fragmentMain", shader_compiler::ShaderStage::Fragment),
        ],
    )
    .expect("compile pipeline-ext shader to SPIR-V");
    let fragment_spirv = modules.pop().expect("fragmentMain bytecode");
    let vertex_spirv = modules.pop().expect("vertexMain bytecode");
    eprintln!(
        "[pipeline-ext] compiled Slang -> SPIR-V: vertexMain={} bytes, fragmentMain={} bytes",
        vertex_spirv.len(),
        fragment_spirv.len()
    );

    let mut rhi = engine.rhi();

    let vertex_module = rhi
        .create_shader_module(&ShaderModuleDesc { language: ShaderLanguage::SpirV, code: vertex_spirv, label: "pipeline-ext-vs".to_string() })
        .expect("create vertex shader module");
    let fragment_module = rhi
        .create_shader_module(&ShaderModuleDesc { language: ShaderLanguage::SpirV, code: fragment_spirv, label: "pipeline-ext-fs".to_string() })
        .expect("create fragment shader module");

    let pipeline_layout = rhi
        .create_pipeline_layout(&PipelineLayoutDesc { bind_group_layouts: vec![], push_constant_ranges: vec![], label: "pipeline-ext-layout".to_string() })
        .expect("create empty pipeline layout");

    // -- Geometry: 3 triangles, interleaved (position: f32x2, color: f32x3) = 20 bytes/vertex -----
    #[rustfmt::skip]
    let vertices: [f32; 9 * 5] = [
        // left / red -- apex at (-0.1, 0.0), so y=0 sits at the triangle's widest interior point
        -0.9, -0.9, 1.0, 0.0, 0.0,
        -0.9,  0.9, 1.0, 0.0, 0.0,
        -0.1,  0.0, 1.0, 0.0, 0.0,
        // right / green -- mirror of the left triangle
         0.1, -0.9, 0.0, 1.0, 0.0,
         0.1,  0.9, 0.0, 1.0, 0.0,
         0.9,  0.0, 0.0, 1.0, 0.0,
        // full-screen / white -- only used by the blend-constant pass
        -1.0, -1.0, 1.0, 1.0, 1.0,
         1.0, -1.0, 1.0, 1.0, 1.0,
         0.0,  1.0, 1.0, 1.0, 1.0,
    ];
    let vertex_bytes: Vec<u8> = vertices.iter().flat_map(|f| f.to_le_bytes()).collect();
    let vertex_buffer = rhi
        .create_buffer(&BufferDesc {
            size: vertex_bytes.len() as u64,
            usage: (BufferUsage::VERTEX | BufferUsage::TRANSFER_DST).bits(),
            memory: MemoryLocation::HostUpload,
            label: "pipeline-ext-vertices".to_string(),
        })
        .expect("create vertex buffer");
    rhi.write_buffer(vertex_buffer, 0, &vertex_bytes).expect("upload vertex buffer");

    // -- Indirect draw buffer: two DrawArgs records, red then green -------------------------------
    let indirect_records = [
        DrawArgs { vertex_count: 3, instance_count: 1, first_vertex: 0, first_instance: 0 },
        DrawArgs { vertex_count: 3, instance_count: 1, first_vertex: 3, first_instance: 0 },
    ];
    let indirect_bytes: Vec<u8> = indirect_records
        .iter()
        .flat_map(|d| [d.vertex_count, d.instance_count, d.first_vertex, d.first_instance])
        .flat_map(|v| v.to_le_bytes())
        .collect();
    let indirect_stride = (indirect_bytes.len() / indirect_records.len()) as u32;
    let indirect_buffer = rhi
        .create_buffer(&BufferDesc {
            size: indirect_bytes.len() as u64,
            usage: (BufferUsage::INDIRECT | BufferUsage::TRANSFER_DST).bits(),
            memory: MemoryLocation::HostUpload,
            label: "pipeline-ext-indirect".to_string(),
        })
        .expect("create indirect buffer");
    rhi.write_buffer(indirect_buffer, 0, &indirect_bytes).expect("upload indirect buffer");

    // -- Render targets: a color texture + a depth/stencil texture, both SIZE x SIZE --------------
    let color_texture = rhi
        .create_texture(&TextureDesc {
            dimension: TextureDimension::Dim2D,
            format: Format::RGBA8Unorm,
            width: SIZE,
            height: SIZE,
            depth_or_layers: 1,
            mip_levels: 1,
            samples: SampleCount::X1,
            usage: (TextureUsage::COLOR_ATTACHMENT | TextureUsage::TRANSFER_SRC).bits(),
            label: "pipeline-ext-color".to_string(),
        })
        .expect("create color target");
    let color_view = rhi
        .create_texture_view(&TextureViewDesc {
            texture: color_texture,
            view_type: TextureViewType::View2D,
            format: Format::RGBA8Unorm,
            base_mip_level: 0,
            mip_level_count: 1,
            base_array_layer: 0,
            array_layer_count: 1,
            label: "pipeline-ext-color-view".to_string(),
        })
        .expect("create color view");

    let depth_texture = rhi
        .create_texture(&TextureDesc {
            dimension: TextureDimension::Dim2D,
            format: Format::D24UnormS8Uint,
            width: SIZE,
            height: SIZE,
            depth_or_layers: 1,
            mip_levels: 1,
            samples: SampleCount::X1,
            usage: TextureUsage::DEPTH_STENCIL_ATTACHMENT.bits(),
            label: "pipeline-ext-depth".to_string(),
        })
        .expect("create depth/stencil target");
    let depth_view = rhi
        .create_texture_view(&TextureViewDesc {
            texture: depth_texture,
            view_type: TextureViewType::View2D,
            format: Format::D24UnormS8Uint,
            base_mip_level: 0,
            mip_level_count: 1,
            base_array_layer: 0,
            array_layer_count: 1,
            label: "pipeline-ext-depth-view".to_string(),
        })
        .expect("create depth view");

    // -- Pipeline A: opaque, depth-bounds test + per-face stencil ops enabled ----------------------
    let vertex_layout = VertexBufferLayout {
        stride: 20,
        step_mode: VertexStepMode::Vertex,
        attributes: vec![
            VertexAttribute { format: VertexFormat::Float32x2, offset: 0, shader_location: 0, semantic_name: "POSITION".to_string(), semantic_index: 0 },
            VertexAttribute { format: VertexFormat::Float32x3, offset: 8, shader_location: 1, semantic_name: "COLOR".to_string(), semantic_index: 0 },
        ],
    };
    let rasterization = RasterizationState {
        polygon_mode: PolygonMode::Fill,
        cull_mode: CullMode::None,
        front_face: FrontFace::CounterClockwise,
        depth_bias_constant: 0.0,
        depth_bias_slope_scale: 0.0,
        depth_bias_clamp: 0.0,
        line_width: 1.0,
    };
    let opaque_color_target = ColorTargetState {
        format: Format::RGBA8Unorm,
        blend_enable: false,
        color: BlendComponent { src_factor: BlendFactor::One, dst_factor: BlendFactor::Zero, op: BlendOp::Add },
        alpha: BlendComponent { src_factor: BlendFactor::One, dst_factor: BlendFactor::Zero, op: BlendOp::Add },
        write_mask: ColorWriteMask::ALL.bits(),
    };
    let stencil_face = StencilFaceState { fail_op: StencilOp::Keep, depth_fail_op: StencilOp::Keep, pass_op: StencilOp::Replace, compare: CompareOp::Always };

    let opaque_pipeline = rhi
        .create_render_pipeline(&RenderPipelineDesc {
            layout: pipeline_layout,
            vertex: ShaderEntry { module: vertex_module, entry_point: "vertexMain".to_string(), stage: ShaderStage::VERTEX.bits() },
            fragment: ShaderEntry { module: fragment_module, entry_point: "fragmentMain".to_string(), stage: ShaderStage::FRAGMENT.bits() },
            vertex_buffers: vec![vertex_layout.clone()],
            topology: PrimitiveTopology::TriangleList,
            rasterization: rasterization.clone(),
            multisample: MultisampleState { samples: SampleCount::X1, sample_mask: !0, alpha_to_coverage_enable: false, sample_locations_enable: false },
            depth_stencil: DepthStencilState {
                format: Format::D24UnormS8Uint,
                depth_test_enable: true,
                depth_write_enable: true,
                depth_compare: CompareOp::Always,
                stencil_test_enable: true,
                stencil_front: stencil_face,
                stencil_back: stencil_face,
                stencil_read_mask: 0xFF,
                stencil_write_mask: 0xFF,
                depth_bounds_test_enable: true,
            },
            color_targets: vec![opaque_color_target],
            label: "pipeline-ext-opaque".to_string(),
        })
        .expect("create opaque render pipeline (depth-bounds + stencil enabled)");

    // -- Pipeline B: blending enabled, no depth/stencil, for the blend-constant pass ---------------
    let blend_pipeline = rhi
        .create_render_pipeline(&RenderPipelineDesc {
            layout: pipeline_layout,
            vertex: ShaderEntry { module: vertex_module, entry_point: "vertexMain".to_string(), stage: ShaderStage::VERTEX.bits() },
            fragment: ShaderEntry { module: fragment_module, entry_point: "fragmentMain".to_string(), stage: ShaderStage::FRAGMENT.bits() },
            vertex_buffers: vec![vertex_layout],
            topology: PrimitiveTopology::TriangleList,
            rasterization,
            multisample: MultisampleState { samples: SampleCount::X1, sample_mask: !0, alpha_to_coverage_enable: false, sample_locations_enable: false },
            depth_stencil: DepthStencilState {
                format: Format::Undefined,
                depth_test_enable: false,
                depth_write_enable: false,
                depth_compare: CompareOp::Always,
                stencil_test_enable: false,
                stencil_front: StencilFaceState { fail_op: StencilOp::Keep, depth_fail_op: StencilOp::Keep, pass_op: StencilOp::Keep, compare: CompareOp::Always },
                stencil_back: StencilFaceState { fail_op: StencilOp::Keep, depth_fail_op: StencilOp::Keep, pass_op: StencilOp::Keep, compare: CompareOp::Always },
                stencil_read_mask: 0,
                stencil_write_mask: 0,
                depth_bounds_test_enable: false,
            },
            color_targets: vec![ColorTargetState {
                format: Format::RGBA8Unorm,
                blend_enable: true,
                color: BlendComponent { src_factor: BlendFactor::ConstantColor, dst_factor: BlendFactor::OneMinusConstantColor, op: BlendOp::Add },
                alpha: BlendComponent { src_factor: BlendFactor::One, dst_factor: BlendFactor::Zero, op: BlendOp::Add },
                write_mask: ColorWriteMask::ALL.bits(),
            }],
            label: "pipeline-ext-blend".to_string(),
        })
        .expect("create blend render pipeline");

    // -- Readback buffer: one RGBA8 texel per check, copied directly (no full-texture copy needed) -
    let readback = rhi
        .create_buffer(&BufferDesc { size: 7 * 4, usage: BufferUsage::TRANSFER_DST.bits(), memory: MemoryLocation::HostReadback, label: "pipeline-ext-readback".to_string() })
        .expect("create readback buffer");

    let mut encoder = rhi
        .create_command_encoder(&CommandEncoderDesc { queue: QueueClass::Graphics, label: "pipeline-ext-encoder".to_string() })
        .expect("create pipeline-ext command encoder");

    let full_range = TextureSubresourceRange { base_mip_level: 0, mip_level_count: 1, base_array_layer: 0, array_layer_count: 1 };
    let color_attachment = |load_op| ColorAttachment {
        view: color_view,
        resolve_view: sturdy::rhi::TextureViewHandle::default(),
        load_op,
        store_op: StoreOp::Store,
        clear_color: ClearColor { r: 0.0, g: 0.0, b: 0.0, a: 1.0 },
    };
    let depth_attachment = DepthStencilAttachment {
        view: depth_view,
        depth_load_op: sturdy::rhi::LoadOp::Clear,
        depth_store_op: StoreOp::Store,
        stencil_load_op: sturdy::rhi::LoadOp::Clear,
        stencil_store_op: StoreOp::Store,
        clear_depth: 1.0,
        clear_stencil: 0,
    };
    let viewport = Viewport { x: 0.0, y: 0.0, width: SIZE as f32, height: SIZE as f32, min_depth: 0.0, max_depth: 1.0 };
    let scissor = Rect2D { x: 0, y: 0, width: SIZE, height: SIZE };

    // Undefined -> attachment layouts, once, before the first pass.
    encoder.barrier(
        &[],
        &[],
        &[
            TextureBarrier {
                texture: color_texture,
                src_stage: PipelineStage::NONE.bits(),
                src_access: 0,
                dst_stage: PipelineStage::COLOR_ATTACHMENT_OUTPUT.bits(),
                dst_access: AccessFlags::COLOR_ATTACHMENT_WRITE.bits(),
                ownership: Default::default(),
                old_layout: TextureLayout::Undefined,
                new_layout: TextureLayout::ColorAttachment,
                range: full_range,
            },
            TextureBarrier {
                texture: depth_texture,
                src_stage: PipelineStage::NONE.bits(),
                src_access: 0,
                dst_stage: PipelineStage::EARLY_FRAGMENT_TESTS.bits(),
                dst_access: AccessFlags::DEPTH_STENCIL_ATTACHMENT_WRITE.bits(),
                ownership: Default::default(),
                old_layout: TextureLayout::Undefined,
                new_layout: TextureLayout::DepthStencilAttachment,
                range: full_range,
            },
        ],
    );

    /// Barriers `color_texture` from `ColorAttachment` to `TransferSrc`, copies one texel out to
    /// `readback` at `slot`, then barriers back to `ColorAttachment` for the next pass.
    fn readback_texel(
        encoder: &mut sturdy::rhi::CommandEncoder,
        color_texture: sturdy::rhi::TextureHandle,
        readback: sturdy::rhi::BufferHandle,
        full_range: TextureSubresourceRange,
        x: u32,
        y: u32,
        slot: u32,
    ) {
        let to_transfer_src = TextureBarrier {
            texture: color_texture,
            src_stage: PipelineStage::COLOR_ATTACHMENT_OUTPUT.bits(),
            src_access: AccessFlags::COLOR_ATTACHMENT_WRITE.bits(),
            dst_stage: PipelineStage::TRANSFER.bits(),
            dst_access: AccessFlags::TRANSFER_READ.bits(),
            ownership: Default::default(),
            old_layout: TextureLayout::ColorAttachment,
            new_layout: TextureLayout::TransferSrc,
            range: full_range,
        };
        encoder.barrier(&[], &[], &[to_transfer_src]);
        encoder.copy_texture_to_buffer(
            color_texture,
            readback,
            &sturdy::rhi::BufferTextureCopy {
                buffer_offset: (slot * 4) as u64,
                buffer_row_length: 1,
                buffer_image_height: 1,
                mip_level: 0,
                base_array_layer: 0,
                array_layer_count: 1,
                offset_x: x as i32,
                offset_y: y as i32,
                offset_z: 0,
                extent_width: 1,
                extent_height: 1,
                extent_depth_or_layers: 1,
            },
        );
        let back_to_color_attachment = TextureBarrier {
            texture: color_texture,
            src_stage: PipelineStage::TRANSFER.bits(),
            src_access: AccessFlags::TRANSFER_READ.bits(),
            dst_stage: PipelineStage::COLOR_ATTACHMENT_OUTPUT.bits(),
            dst_access: AccessFlags::COLOR_ATTACHMENT_WRITE.bits(),
            ownership: Default::default(),
            old_layout: TextureLayout::TransferSrc,
            new_layout: TextureLayout::ColorAttachment,
            range: full_range,
        };
        encoder.barrier(&[], &[], &[back_to_color_attachment]);
    }

    // -- Pass 1: draw_indirect_multi(draw_count=1) + set_stencil_reference + set_depth_bounds(pass) -
    {
        let mut rp = encoder
            .begin_render_pass(&RenderPassDesc {
                color_attachments: vec![color_attachment(sturdy::rhi::LoadOp::Clear)],
                has_depth_stencil: true,
                depth_stencil: depth_attachment.clone(),
                render_area: scissor.clone(),
                label: "pipeline-ext-pass1".to_string(),
            })
            .expect("begin pass 1");
        rp.set_viewport(&viewport);
        rp.set_scissor(&scissor);
        rp.set_pipeline(opaque_pipeline);
        rp.set_vertex_buffer(0, vertex_buffer, 0);
        rp.set_stencil_reference(7);
        rp.set_depth_bounds(0.0, 1.0);
        rp.draw_indirect_multi(indirect_buffer, 0, 1, indirect_stride);
    }
    readback_texel(&mut encoder, color_texture, readback, full_range, LEFT_COL, MID_ROW, 0);

    // -- Pass 2: draw_indirect_multi(draw_count=2) -- both triangles from one call ------------------
    {
        let mut rp = encoder
            .begin_render_pass(&RenderPassDesc {
                color_attachments: vec![color_attachment(sturdy::rhi::LoadOp::Clear)],
                has_depth_stencil: true,
                depth_stencil: depth_attachment.clone(),
                render_area: scissor.clone(),
                label: "pipeline-ext-pass2".to_string(),
            })
            .expect("begin pass 2");
        rp.set_viewport(&viewport);
        rp.set_scissor(&scissor);
        rp.set_pipeline(opaque_pipeline);
        rp.set_vertex_buffer(0, vertex_buffer, 0);
        rp.set_stencil_reference(7);
        rp.set_depth_bounds(0.0, 1.0);
        rp.draw_indirect_multi(indirect_buffer, 0, 2, indirect_stride);
    }
    readback_texel(&mut encoder, color_texture, readback, full_range, LEFT_COL, MID_ROW, 1);
    readback_texel(&mut encoder, color_texture, readback, full_range, RIGHT_COL, MID_ROW, 2);

    // -- Pass 3: bounds [0.0, 0.5] must reject the whole draw -----------------------------------------
    //
    // The depth-bounds test compares against whatever is *currently stored* in the depth
    // attachment at that pixel -- not the incoming fragment's own depth -- confirmed empirically
    // while writing this test: an earlier version used bounds [0.5, 1.0] expecting z=0 geometry to
    // be rejected, but the depth attachment was cleared to 1.0 (which *is* inside [0.5, 1.0]), so
    // the bounds test passed regardless of the triangle's own depth. Clearing to 1.0 and testing
    // against [0.0, 0.5] (which excludes the cleared value) is what actually exercises rejection.
    {
        let mut rp = encoder
            .begin_render_pass(&RenderPassDesc {
                color_attachments: vec![color_attachment(sturdy::rhi::LoadOp::Clear)],
                has_depth_stencil: true,
                depth_stencil: depth_attachment.clone(),
                render_area: scissor.clone(),
                label: "pipeline-ext-pass3".to_string(),
            })
            .expect("begin pass 3");
        rp.set_viewport(&viewport);
        rp.set_scissor(&scissor);
        rp.set_pipeline(opaque_pipeline);
        rp.set_vertex_buffer(0, vertex_buffer, 0);
        rp.set_stencil_reference(7);
        rp.set_depth_bounds(0.0, 0.5);
        rp.draw(&DrawArgs { vertex_count: 3, instance_count: 1, first_vertex: 0, first_instance: 0 });
    }
    readback_texel(&mut encoder, color_texture, readback, full_range, LEFT_COL, MID_ROW, 3);

    // -- Pass 4: blend pipeline, white triangle over black clear, exact blend-constant result -------
    {
        let mut rp = encoder
            .begin_render_pass(&RenderPassDesc {
                color_attachments: vec![color_attachment(sturdy::rhi::LoadOp::Clear)],
                has_depth_stencil: false,
                depth_stencil: DepthStencilAttachment {
                    view: sturdy::rhi::TextureViewHandle::default(),
                    depth_load_op: sturdy::rhi::LoadOp::DontCare,
                    depth_store_op: StoreOp::DontCare,
                    stencil_load_op: sturdy::rhi::LoadOp::DontCare,
                    stencil_store_op: StoreOp::DontCare,
                    clear_depth: 1.0,
                    clear_stencil: 0,
                },
                render_area: scissor.clone(),
                label: "pipeline-ext-pass4".to_string(),
            })
            .expect("begin pass 4");
        rp.set_viewport(&viewport);
        rp.set_scissor(&scissor);
        rp.set_pipeline(blend_pipeline);
        rp.set_vertex_buffer(0, vertex_buffer, 0);
        rp.set_blend_constant(&ClearColor { r: 0.25, g: 0.25, b: 0.25, a: 0.25 });
        rp.draw(&DrawArgs { vertex_count: 3, instance_count: 1, first_vertex: 6, first_instance: 0 });
    }
    readback_texel(&mut encoder, color_texture, readback, full_range, CENTER_COL, MID_ROW, 4);

    // -- Pass 5: a render bundle, recorded once up front, replayed via execute_bundles ------------
    //
    // The bundle carries *all* of its own state (viewport, scissor, pipeline, vertex buffer,
    // dynamic state) and draws only the green triangle; the pass itself does nothing but replay
    // it. Green on the right and the black clear on the left proves the bundle's recorded draw --
    // and only that draw -- actually executed.
    let bundle = {
        let mut rb = rhi
            .create_render_bundle_encoder(&sturdy::rhi::RenderBundleDesc {
                color_formats: vec![Format::RGBA8Unorm],
                depth_stencil_format: Format::D24UnormS8Uint,
                samples: SampleCount::X1,
                view_mask: 0,
                label: "pipeline-ext-bundle".to_string(),
            })
            .expect("create render bundle encoder");
        rb.set_viewport(&viewport);
        rb.set_scissor(&scissor);
        rb.set_pipeline(opaque_pipeline);
        rb.set_vertex_buffer(0, vertex_buffer, 0);
        rb.set_stencil_reference(7);
        rb.set_depth_bounds(0.0, 1.0);
        rb.draw(&DrawArgs { vertex_count: 3, instance_count: 1, first_vertex: 3, first_instance: 0 });
        rb.finish().expect("finish render bundle")
    };
    {
        let mut rp = encoder
            .begin_render_pass(&RenderPassDesc {
                color_attachments: vec![color_attachment(sturdy::rhi::LoadOp::Clear)],
                has_depth_stencil: true,
                depth_stencil: depth_attachment,
                render_area: scissor.clone(),
                label: "pipeline-ext-pass5-bundle".to_string(),
            })
            .expect("begin pass 5");
        rp.execute_bundles(&[bundle]);
    }
    readback_texel(&mut encoder, color_texture, readback, full_range, LEFT_COL, MID_ROW, 5);
    readback_texel(&mut encoder, color_texture, readback, full_range, RIGHT_COL, MID_ROW, 6);

    // -- Submit with a fence + a GPU-signaled timeline semaphore, instead of wait_idle ----------
    let fence = rhi
        .create_fence(&sturdy::rhi::FenceDesc { signaled: false, label: "pipeline-ext-fence".to_string() })
        .expect("create fence");
    let timeline = rhi
        .create_semaphore(&sturdy::rhi::SemaphoreDesc { initial_value: 0, label: "pipeline-ext-timeline".to_string() })
        .expect("create timeline semaphore");

    // CPU-side signal/read round trip before any GPU involvement.
    rhi.signal_semaphore(timeline, 5).expect("CPU-signal timeline semaphore");
    let cpu_signaled = rhi.semaphore_value(timeline).expect("read timeline semaphore");
    eprintln!("[pipeline-ext] timeline semaphore after CPU signal(5) = {cpu_signaled} (expected 5)");
    assert_eq!(cpu_signaled, 5);

    let command_buffer = encoder.finish().expect("finish pipeline-ext command buffer");
    rhi.submit_with(&sturdy::rhi::SubmitDesc {
        queue: sturdy::rhi::QueueLane::default(),
        command_buffers: vec![command_buffer],
        waits: vec![sturdy::rhi::QueueSemaphoreOp { semaphore: timeline, value: 5, stages: PipelineStage::ALL_COMMANDS.bits() }],
        signals: vec![sturdy::rhi::QueueSemaphoreOp { semaphore: timeline, value: 10, stages: PipelineStage::ALL_COMMANDS.bits() }],
        fence,
        one_shot: true,
        label: "pipeline-ext-submit".to_string(),
    })
    .expect("submit_with pipeline-ext command buffer");

    let completed = rhi.wait_fences(&[fence], true, sturdy::rhi::WAIT_FOREVER).expect("wait on pipeline-ext fence");
    eprintln!("[pipeline-ext] wait_fences after submit_with = {completed} (expected true)");
    assert!(completed, "the submission's fence must signal");
    rhi.wait_semaphore(timeline, 10, sturdy::rhi::WAIT_FOREVER).expect("wait for GPU-signaled timeline value");
    let gpu_signaled = rhi.semaphore_value(timeline).expect("read timeline semaphore after submit");
    eprintln!("[pipeline-ext] timeline semaphore after the submission's signal(10) = {gpu_signaled} (expected 10)");
    assert_eq!(gpu_signaled, 10, "the submission must have signaled the timeline semaphore");

    rhi.reset_fences(&[fence]).expect("reset fence");
    let reset_poll = rhi.wait_fences(&[fence], true, 0).expect("poll reset fence");
    eprintln!("[pipeline-ext] zero-timeout poll of the fence after reset_fences = {reset_poll} (expected false)");
    assert!(!reset_poll, "a reset fence must read as unsignaled");
    rhi.destroy_fence(fence);
    rhi.destroy_semaphore(timeline);
    rhi.destroy_render_bundle(bundle);

    {
        let mapped = rhi.map_buffer(readback).expect("map pipeline-ext readback buffer");
        let texel = |slot: usize| -> [u8; 4] { mapped[slot * 4..slot * 4 + 4].try_into().unwrap() };

        let pass1_red = texel(0);
        eprintln!("[pipeline-ext] pass 1 (draw_indirect_multi, count=1) left pixel = {pass1_red:?} (expected ~[255,0,0,255])");
        assert!(pass1_red[0] > 200 && pass1_red[1] < 50 && pass1_red[2] < 50, "pass 1 must show the red triangle");

        let pass2_red = texel(1);
        let pass2_green = texel(2);
        eprintln!(
            "[pipeline-ext] pass 2 (draw_indirect_multi, count=2) left={pass2_red:?} right={pass2_green:?} (expected ~[255,0,0,255] / ~[0,255,0,255])"
        );
        assert!(pass2_red[0] > 200 && pass2_red[1] < 50, "pass 2 must show the red triangle on the left");
        assert!(pass2_green[1] > 200 && pass2_green[0] < 50, "pass 2 must show the green triangle on the right -- proves draw_count=2 issued two real draws, not one");

        let pass3_bg = texel(3);
        eprintln!("[pipeline-ext] pass 3 (set_depth_bounds(0.0, 0.5) against a depth attachment cleared to 1.0) left pixel = {pass3_bg:?} (expected the black clear color, [0,0,0,255] -- the whole draw must be rejected)");
        assert!(pass3_bg[0] < 20 && pass3_bg[1] < 20 && pass3_bg[2] < 20, "set_depth_bounds must actually reject out-of-range geometry, not just accept the call");

        let pass4_blend = texel(4);
        eprintln!("[pipeline-ext] pass 4 (set_blend_constant(0.25)) center pixel = {pass4_blend:?} (expected ~[64,64,64,255] = white * 0.25 + black * 0.75)");
        for channel in 0..3 {
            let value = pass4_blend[channel] as i32;
            assert!((value - 64).abs() <= 10, "blend-constant result channel {channel} = {value}, expected close to 64");
        }

        let pass5_left = texel(5);
        let pass5_right = texel(6);
        eprintln!(
            "[pipeline-ext] pass 5 (execute_bundles, bundle draws only the green triangle) left={pass5_left:?} right={pass5_right:?} (expected [0,0,0,255] / ~[0,255,0,255])"
        );
        assert!(pass5_left[0] < 20 && pass5_left[1] < 20, "pass 5 left must be the clear color -- the bundle only draws green");
        assert!(pass5_right[1] > 200 && pass5_right[0] < 50, "pass 5 right must be green -- the bundle's recorded draw must replay");
    }

    rhi.destroy_buffer(readback);
    rhi.destroy_render_pipeline(blend_pipeline);
    rhi.destroy_render_pipeline(opaque_pipeline);
    rhi.destroy_texture_view(depth_view);
    rhi.destroy_texture(depth_texture);
    rhi.destroy_texture_view(color_view);
    rhi.destroy_texture(color_texture);
    rhi.destroy_buffer(indirect_buffer);
    rhi.destroy_buffer(vertex_buffer);
    rhi.destroy_pipeline_layout(pipeline_layout);
    rhi.destroy_shader_module(fragment_module);
    rhi.destroy_shader_module(vertex_module);

    eprintln!("[pipeline-ext] all render-pipeline-extension demonstration checks passed");
}

/// Exercises the asset-management extensions added on top of the original load/query/unload
/// surface: `create_texture` (raw pixel buffer -> texture, no file decode), `create_orm_texture`
/// (packed occlusion/roughness/metallic), `texture_handle` (Renderer-level handle readback), and
/// `file_bytes` (raw buffer readback for an already-loaded asset). Logged with an `[assets-ext]`
/// prefix. Gated the same way as `demonstrate_rhi_ext`: `AssetManager` is constructed from a
/// `Renderer&`, so this needs to run once a GPU is actually up, not from `init`.
fn demonstrate_assets_ext(engine: &mut Engine<'_>) {
    use sturdy::assets::{TextureColorSpace, TextureDynamicRange, TextureKind, TexturePixelFormat};

    let mut assets = engine.assets();

    // -- create_texture: a procedural 4x4 checkerboard, straight from a raw RGBA8 pixel buffer,
    // no image decoder involved. Verified by reading the dimensions/color-space back through
    // `texture_info` (the only readback this asset kind exposes -- there is no raw-pixel readback
    // API for GPU textures, unlike `file_bytes`/`sound_samples` for File/Sound assets).
    const SIZE: u32 = 4;
    let mut pixels = vec![0u8; (SIZE * SIZE * 4) as usize];
    for y in 0..SIZE {
        for x in 0..SIZE {
            let idx = ((y * SIZE + x) * 4) as usize;
            let color: [u8; 4] = if (x + y) % 2 == 0 { [255, 32, 32, 255] } else { [32, 32, 255, 255] };
            pixels[idx..idx + 4].copy_from_slice(&color);
        }
    }
    let texture = assets
        .create_texture(
            &pixels,
            SIZE,
            SIZE,
            TexturePixelFormat::Rgba8,
            TextureColorSpace::Linear,
            TextureKind::ColorAlpha,
            "assets-ext-checkerboard",
            false, // allow_compression
            false, // generate_mipmaps
        )
        .expect("create_texture from a raw 4x4 RGBA8 checkerboard buffer");
    eprintln!("[assets-ext] create_texture: asset={texture:?}");

    let info = assets.texture_info(texture).expect("texture_info for the procedural checkerboard");
    eprintln!(
        "[assets-ext] texture_info: {}x{} color_space={:?} (expected {SIZE}x{SIZE} Linear)",
        info.size.x, info.size.y, info.color_space
    );
    assert_eq!(info.size, glam::uvec2(SIZE, SIZE));
    assert_eq!(info.color_space, TextureColorSpace::Linear);

    // -- texture_handle: the Renderer-registry handle backing that same texture asset.
    let renderer_handle = assets.texture_handle(texture).expect("texture_handle for the procedural checkerboard");
    eprintln!("[assets-ext] texture_handle: {renderer_handle:?} (expected a valid/nonzero handle)");
    assert!(renderer_handle.is_valid(), "a freshly created texture must have a valid Renderer-level handle");

    // -- rhi_texture: resolve that Renderer handle to its RHI texture, then read the texels back
    // through plain RHI calls -- proving it's the real GPU resource holding the checkerboard.
    let resources = assets.rhi_texture(renderer_handle).expect("rhi_texture for a live texture");
    eprintln!(
        "[assets-ext] rhi_texture: texture={:?} view={:?} sampler={:?} {}x{} mips={} format={:?}",
        resources.texture, resources.view, resources.sampler, resources.size.x, resources.size.y, resources.mip_levels, resources.format
    );
    assert_eq!(resources.size, glam::uvec2(SIZE, SIZE));
    assert!(resources.texture.value != 0 && resources.view.value != 0);
    assert!(assets.rhi_texture(sturdy::assets::RendererTextureHandle::default()).is_none());
    drop(assets);
    {
        use sturdy::rhi::{AccessFlags, BufferDesc, BufferUsage, CommandEncoderDesc, MemoryLocation, PipelineStage, QueueClass, TextureBarrier, TextureLayout, TextureSubresourceRange};
        let mut rhi = engine.rhi();
        let readback = rhi
            .create_buffer(&BufferDesc { size: (SIZE * SIZE * 4) as u64, usage: BufferUsage::TRANSFER_DST.bits(), memory: MemoryLocation::HostReadback, label: "assets-ext-readback".to_string() })
            .expect("create readback buffer");
        let mut encoder = rhi
            .create_command_encoder(&CommandEncoderDesc { queue: QueueClass::Graphics, label: "assets-ext-readback".to_string() })
            .expect("create command encoder");
        let range = TextureSubresourceRange { base_mip_level: 0, mip_level_count: 1, base_array_layer: 0, array_layer_count: 1 };
        let barrier = |old_layout, new_layout, src_access: AccessFlags, dst_access: AccessFlags| TextureBarrier {
            texture: resources.texture,
            src_stage: PipelineStage::ALL_COMMANDS.bits(),
            src_access: src_access.bits(),
            dst_stage: PipelineStage::ALL_COMMANDS.bits(),
            dst_access: dst_access.bits(),
            ownership: Default::default(),
            old_layout,
            new_layout,
            range,
        };
        encoder.barrier(&[], &[], &[barrier(TextureLayout::ShaderReadOnly, TextureLayout::TransferSrc, AccessFlags::SHADER_READ, AccessFlags::TRANSFER_READ)]);
        encoder.copy_texture_to_buffer(
            resources.texture,
            readback,
            &sturdy::rhi::BufferTextureCopy {
                buffer_offset: 0,
                buffer_row_length: SIZE,
                buffer_image_height: SIZE,
                mip_level: 0,
                base_array_layer: 0,
                array_layer_count: 1,
                offset_x: 0,
                offset_y: 0,
                offset_z: 0,
                extent_width: SIZE,
                extent_height: SIZE,
                extent_depth_or_layers: 1,
            },
        );
        encoder.barrier(&[], &[], &[barrier(TextureLayout::TransferSrc, TextureLayout::ShaderReadOnly, AccessFlags::TRANSFER_READ, AccessFlags::SHADER_READ)]);
        let commands = encoder.finish().expect("finish readback encoder");
        rhi.submit(&[commands]).expect("submit readback");
        rhi.wait_idle();
        let matches = {
            let mapped = rhi.map_buffer(readback).expect("map readback");
            let matches = mapped[..pixels.len()] == pixels[..];
            eprintln!("[assets-ext] RHI readback of the asset texture: first texels {:?} -> matches uploaded checkerboard = {matches}", &mapped[0..8]);
            matches
        };
        rhi.destroy_buffer(readback);
        assert!(matches, "the RHI texture behind the asset must hold the uploaded pixels");
    }
    let mut assets = engine.assets();

    // -- create_orm_texture: pack a solid-white occlusion buffer and a metallic-roughness buffer
    // (roughness=128, metallic=255 in every texel) into one ORM texture asset.
    let orm_size = 2u32;
    let occlusion_rgba8 = vec![255u8; (orm_size * orm_size * 4) as usize];
    let mut metallic_roughness_rgba8 = vec![0u8; (orm_size * orm_size * 4) as usize];
    for texel in metallic_roughness_rgba8.chunks_exact_mut(4) {
        texel[0] = 128; // roughness
        texel[1] = 0;
        texel[2] = 255; // metallic
        texel[3] = 255;
    }
    let orm_texture = assets
        .create_orm_texture(&occlusion_rgba8, &metallic_roughness_rgba8, orm_size, orm_size, "assets-ext-orm")
        .expect("create_orm_texture from occlusion + metallic-roughness buffers");
    let orm_info = assets.texture_info(orm_texture).expect("texture_info for the ORM texture");
    eprintln!(
        "[assets-ext] create_orm_texture: asset={orm_texture:?}, texture_info={}x{} color_space={:?} (expected {orm_size}x{orm_size})",
        orm_info.size.x, orm_info.size.y, orm_info.color_space
    );
    assert_eq!(orm_info.size, glam::uvec2(orm_size, orm_size));

    // -- file_bytes: load this very source file as a raw `File` asset, then confirm the bytes
    // read back through the bridge match what's actually on disk.
    let this_file = concat!(env!("CARGO_MANIFEST_DIR"), "/src/main.rs");
    let on_disk = std::fs::read(this_file).expect("read main.rs from disk for comparison");
    let file_asset = assets.load_file(this_file, "assets-ext-main-rs").expect("load_file(main.rs)");
    let read_back = assets.file_bytes(file_asset).expect("file_bytes for the loaded File asset");
    eprintln!(
        "[assets-ext] file_bytes: loaded {} bytes, on-disk {} bytes (expected equal)",
        read_back.len(),
        on_disk.len()
    );
    assert_eq!(read_back, on_disk, "file_bytes must return exactly what's on disk");

    // Also prove create_texture rejects a garbage size the way the doc comment says it should
    // (mismatched pixel buffer length is caught by the engine, not just silently truncated).
    let bad = assets.create_texture(
        &pixels[..pixels.len() - 1],
        SIZE,
        SIZE,
        TexturePixelFormat::Rgba8,
        TextureColorSpace::Linear,
        TextureKind::ColorAlpha,
        "assets-ext-bad-size",
        false,
        false,
    );
    eprintln!("[assets-ext] create_texture with a truncated pixel buffer -> {bad:?} (expected Err)");
    assert!(bad.is_err(), "a pixel buffer shorter than width*height*4 must be rejected");

    assets.unload(texture).expect("unload procedural checkerboard texture");
    assets.unload(orm_texture).expect("unload ORM texture");
    assets.unload(file_asset).expect("unload main.rs File asset");

    // `TextureDynamicRange`/`load_texture`'s HDR path is exercised by `create_texture`'s sibling
    // API already covered above (`load_texture`/`create_texture_from_bytes`) in earlier passes of
    // this binding; nothing further to demonstrate here.
    let _ = TextureDynamicRange::Sdr;

    eprintln!("[assets-ext] all asset-management-extension demonstration checks passed");
}

/// Imports a real glTF file (`assets/box.gltf` -- see `assets/README.md` for provenance/license)
/// through `Assets::import_gltf`, then spawns it into the ECS world with `GltfScene::spawn_all`
/// and confirms the resulting entities carry the expected components. Logged with a `[gltf-ext]`
/// prefix. Gated the same way as `demonstrate_assets_ext`: needs a live GPU.
fn demonstrate_gltf_ext(engine: &mut Engine<'_>) {
    use sturdy::scene::{ModelRenderer, WorldTransform};

    const BOX_GLTF: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/assets/box.gltf");

    // The importer needs a real shader asset for every primitive (an invalid handle is rejected with
    // "Model primitive references an invalid shader asset"); the engine's own G-buffer shader is
    // embedded in the binary, so this needs no file on disk.
    let shader = engine
        .assets()
        .load_shader("Shaders/gbuffer_geometry.slang", "gltf-ext-shader")
        .expect("load embedded gbuffer shader");
    let scene = engine.assets().import_gltf(BOX_GLTF, shader).expect("import assets/box.gltf");
    eprintln!(
        "[gltf-ext] import_gltf(box.gltf) -> {} models, {} instances, {} lights (expected 1, 1, 0)",
        scene.models.len(),
        scene.instances.len(),
        scene.lights.len()
    );
    assert_eq!(scene.models.len(), 1);
    assert_eq!(scene.instances.len(), 1);
    assert!(scene.lights.is_empty(), "the Box sample has no lights");
    assert!(scene.models[0].is_valid());
    eprintln!(
        "[gltf-ext] instance \"{}\" -> model={:?} world_transform.translation={:?}",
        scene.instances[0].name,
        scene.instances[0].model,
        scene.instances[0].world_transform.transform_point3(glam::Vec3::ZERO)
    );

    let mut world = engine.ecs();
    let entities = scene.spawn_all(&mut world).expect("spawn_all the imported glTF scene");
    eprintln!("[gltf-ext] spawn_all -> {} entities (expected 1)", entities.len());
    assert_eq!(entities.len(), 1);

    let transform: WorldTransform = world.get(entities[0]).expect("read spawned WorldTransform");
    let renderer: ModelRenderer = world.get(entities[0]).expect("read spawned ModelRenderer");
    eprintln!(
        "[gltf-ext] spawned entity {:?}: WorldTransform.translation={:?}, ModelRenderer.model={:?} visible={}",
        entities[0],
        transform.translation(),
        renderer.model,
        renderer.visible()
    );
    assert_eq!(renderer.model.asset(), scene.models[0]);
    assert!(renderer.visible());

    eprintln!("[gltf-ext] all glTF-import-extension demonstration checks passed");
}

/// Queues every window runtime-mutation request (`sturdy::render`'s `set_cursor_icon`/
/// `set_cursor_grabbed`/`set_window_mode`/`set_decorated`/`set_transparent`/
/// `set_relative_mouse_mode`/`set_text_input_active`/`set_text_input_area`/`set_window_effect`)
/// against the primary window, then `set_mouse_locked(true)` last. All of these are
/// enqueue-and-forget (queued via the engine's own `WindowRequests`, applied on its next update --
/// see `sturdy-sys`'s doc comment on them) with no per-call result to check, so most are just
/// exercised and logged; `set_mouse_locked` is the one with an independent readback
/// (`Diagnostics::window`'s `mouse_locked` snapshot field), so `check_window_mutation_demo` polls
/// for that one actually landing.
fn demonstrate_window_mutation(engine: &mut Engine<'_>, surface: SurfaceHandle) {
    engine.set_cursor_icon(surface, CursorIcon::Pointer);
    engine.set_cursor_grabbed(surface, false);
    engine.set_window_mode(surface, sturdy::WindowMode::Windowed);
    engine.set_decorated(surface, true);
    engine.set_transparent(surface, false);
    engine.set_relative_mouse_mode(surface, false);
    engine.set_window_effect(surface, WindowEffectKind::Blur, false);
    engine.set_text_input_active(surface, false);
    engine.set_text_input_area(surface, TextInputArea { position: glam::vec2(10.0, 10.0), size: glam::vec2(100.0, 20.0), cursor_offset_x: 0.0 });
    eprintln!(
        "[window-ext] queued cursor-icon/cursor-grab/window-mode/decorated/transparent/relative-mouse/effect/text-input requests"
    );

    engine.set_mouse_locked(surface, true);
    eprintln!("[window-ext] queued set_mouse_locked(true); verifying via diagnostics().window(0) from frame()");
}

/// Polled from `GameLogic::frame` until `demonstrate_window_mutation`'s `set_mouse_locked(true)`
/// request has visibly applied (via `Diagnostics::window`'s `mouse_locked` field), or a generous
/// frame budget runs out. Immediately queues `set_mouse_locked(false)` once confirmed, so the demo
/// doesn't leave the real OS mouse cursor locked for the rest of the process. Returns `true` once
/// done, so the caller can stop polling.
fn check_window_mutation_demo(engine: &mut Engine<'_>, surface: SurfaceHandle, frames_waited: u32) -> bool {
    let locked = engine.diagnostics().window(0).map(|info| info.mouse_locked).unwrap_or(false);
    if !locked {
        assert!(
            frames_waited < 300,
            "[window-ext] set_mouse_locked(true) never applied after {frames_waited} frames"
        );
        return false;
    }
    eprintln!("[window-ext] confirmed mouse_locked=true via diagnostics().window(0) after {frames_waited} frame(s)");
    engine.set_mouse_locked(surface, false);
    eprintln!("[window-ext] queued set_mouse_locked(false) to restore normal mouse behavior");
    eprintln!("[window-ext] all window-mutation demonstration checks passed");
    true
}

/// Exercises `sturdy::device` against the live engine: enumeration, an in-place device rebuild, a typed
/// failure that leaves the running device alone, and the missing-saved-adapter fallback. Rendering must keep
/// going afterwards (checked by waiting for later frames), which is the real proof the swapchain, assets and
/// UI survived the switch.
#[derive(Default)]
struct DeviceDemo {
    stage: u8,
    switched_at_frame: u64,
    generation_after: u64,
}

impl DeviceDemo {
    fn step(&mut self, engine: &mut Engine<'_>, frame_index: u64) {
        use sturdy::device::{Backend, DeviceChangeKind, DeviceRequest, SwitchOutcome};
        match self.stage {
            0 if frame_index >= 120 => {
                let backends = engine.available_backends();
                let adapters = engine.available_adapters();
                assert!(!backends.is_empty() && !adapters.is_empty(), "[device-switch] nothing enumerated");
                let active: Vec<_> = adapters
                    .iter()
                    .flat_map(|a| a.backends.iter().filter(|b| b.active).map(move |b| (a, b)))
                    .collect();
                assert_eq!(active.len(), 1, "[device-switch] exactly one backend offer should be active");
                let (adapter, offer) = active[0];
                eprintln!(
                    "[device-switch] {} adapter(s), backends {backends:?}; running on {} via {:?} (stable id {})",
                    adapters.len(),
                    adapter.name,
                    offer.backend,
                    adapter.stable_id
                );
                let before = engine.device_generation();

                // A typed failure that must leave the running device untouched.
                let impossible = if backends.contains(&Backend::Metal) { Backend::D3D12 } else { Backend::Metal };
                if !backends.contains(&impossible) {
                    let failed = engine.switch_device(&DeviceRequest::new().backend(impossible));
                    assert!(matches!(failed.outcome, SwitchOutcome::Failed(_)), "[device-switch] {failed:?}");
                    assert_eq!(failed.generation, before, "[device-switch] a refused request rebuilt the device");
                    assert_eq!(failed.backend, Some(offer.backend));
                    eprintln!("[device-switch] unsupported {impossible:?} refused, device untouched");
                }

                // A saved adapter that is gone degrades to the profile with a reason; same device, so no rebuild.
                let fallback = engine.switch_device(&DeviceRequest::new().adapter("0000000000000000"));
                assert!(
                    matches!(fallback.outcome, SwitchOutcome::SwitchedWithFallback(_)),
                    "[device-switch] {fallback:?}"
                );
                eprintln!("[device-switch] missing saved adapter -> {:?}", fallback.outcome);

                // The real thing: tear the device down and rebuild it in place.
                let started = std::time::Instant::now();
                let rebuilt = engine.switch_device(&DeviceRequest::new().adapter(adapter.stable_id.clone()).force_recreate(true));
                assert_eq!(rebuilt.outcome, SwitchOutcome::Switched, "[device-switch] {rebuilt:?}");
                assert!(rebuilt.generation > before, "[device-switch] generation did not advance");
                assert!(!rebuilt.rolled_back);
                eprintln!("[device-switch] rebuilt in {:?}: generation {before} -> {}", started.elapsed(), rebuilt.generation);
                self.generation_after = rebuilt.generation;
                self.switched_at_frame = frame_index;
                self.stage = 1;
            }
            1 if frame_index >= self.switched_at_frame + 60 => {
                // 60 more frames rendered on the new device without a panic or a failed present.
                let events = engine.take_device_change_events();
                assert!(
                    events.iter().any(|e| e.kind == DeviceChangeKind::Switched && e.generation == self.generation_after),
                    "[device-switch] no Switched event: {events:?}"
                );
                assert_eq!(engine.device_generation(), self.generation_after, "[device-switch] device changed again");
                eprintln!("[device-switch] 60 frames rendered after the switch; all device-switch checks passed");
                self.stage = 2;
            }
            _ => {}
        }
    }
}

/// Per-frame results of `install_ui_draw_demo`'s draw hook, read back from `frame()`. Atomics
/// because the hook is a `Send + 'static` closure the engine calls on its own schedule.
struct UiDemoResults {
    frames: std::sync::atomic::AtomicU32,
    stroke_paths_ok: std::sync::atomic::AtomicBool,
    fill_quads_ok: std::sync::atomic::AtomicBool,
    fill_sectors_ok: std::sync::atomic::AtomicBool,
    stroke_custom_ok: std::sync::atomic::AtomicBool,
    /// Set once `scroll_metrics` reports the floating element at its declared 80x30 size.
    floating_laid_out: std::sync::atomic::AtomicBool,
    graph_line_drawn: std::sync::atomic::AtomicBool,
    graph_pie_drawn: std::sync::atomic::AtomicBool,
    /// `add_panel` results: a, b, c, then a duplicate "a" (expected true, true, true, false).
    dock_add_results: std::sync::Mutex<Vec<bool>>,
    /// Panels whose content region opened in the latest frame.
    dock_visible: std::sync::Mutex<Vec<String>>,
    /// Set once an element built inside a docked panel's content shows up in the layout.
    dock_content_laid_out: std::sync::atomic::AtomicBool,
}

/// Installs a UI draw hook exercising the raw UI draw primitives -- `Ui::stroke_paths`/
/// `stroke_polyline` (solid + dashed), `fill_quads` (rounded), `fill_sectors` (a donut chart), and
/// `stroke_custom` through the engine's own `Shaders/ui_stroke_custom_demo.slang` (a 16-byte
/// push-constant tail: `time` + 3 pads, after the engine's 48-byte stroke prefix). Each call's
/// success is recorded in the returned `UiDemoResults` for `check_ui_draw_demo` to assert on.
///
/// Logged with a `[ui-ext]` prefix. Pixel output isn't read back (the UI overlay renders into the
/// swapchain, not a texture this binding can copy from); what's checked is that every call reached
/// an open UI session and the engine accepted it, across several real frames.
fn install_ui_draw_demo() -> Arc<UiDemoResults> {
    use sturdy::ui::{
        AttachPoint, Border, ChildAlignment, Clip, CornerRadius, Cursor, CustomShader, ElementDesc,
        FillQuad, FloatTarget, Floating, GraphAxis, GraphDesc, GraphSeries, GraphType, PieSlice,
        Sector, Sizing, StrokeCap, StrokePath, StrokeStyle, DockPlacement, DockWorkspace, DockZone,
    };
    use std::sync::atomic::Ordering;

    let results = Arc::new(UiDemoResults {
        frames: std::sync::atomic::AtomicU32::new(0),
        stroke_paths_ok: std::sync::atomic::AtomicBool::new(false),
        fill_quads_ok: std::sync::atomic::AtomicBool::new(false),
        fill_sectors_ok: std::sync::atomic::AtomicBool::new(false),
        stroke_custom_ok: std::sync::atomic::AtomicBool::new(false),
        floating_laid_out: std::sync::atomic::AtomicBool::new(false),
        graph_line_drawn: std::sync::atomic::AtomicBool::new(false),
        graph_pie_drawn: std::sync::atomic::AtomicBool::new(false),
        dock_add_results: std::sync::Mutex::new(Vec::new()),
        dock_visible: std::sync::Mutex::new(Vec::new()),
        dock_content_laid_out: std::sync::atomic::AtomicBool::new(false),
    });
    let hook_results = results.clone();
    let start = Instant::now();

    // Docking: "a" and "b" share the root leaf as tabs ("b" becomes the visible one), "c" splits
    // off to the right of that leaf (node 0 is the root leaf of a fresh tree).
    let mut dock = DockWorkspace::new("ui-ext-dock");
    {
        let mut adds = results.dock_add_results.lock().unwrap();
        adds.push(dock.add_panel("dock-a", "A", true, None));
        adds.push(dock.add_panel("dock-b", "B", true, None));
        adds.push(dock.add_panel("dock-c", "C", false, Some(DockPlacement { target_node: 0, zone: DockZone::Right })));
        adds.push(dock.add_panel("dock-a", "A again", true, None));
    }

    let mut widget_demo = WidgetDemo::new();
    sturdy::ui::install_draw_hook(move |ui| {
        let panel = ElementDesc::new()
            .id("ui-ext-panel".to_string())
            .width(Sizing::Fixed(220.0))
            .height(Sizing::Fixed(120.0))
            .align(ChildAlignment::center())
            .border(Border::all(glam::vec4(1.0, 1.0, 1.0, 0.6), 1))
            .cursor(Cursor::Pointer)
            .debug_label("ui-ext panel".to_string());

        let chart = ElementDesc::new().width(Sizing::Fixed(200.0)).height(Sizing::Fixed(100.0));
        let axis = StrokePath {
            points: vec![glam::vec2(0.0, 100.0), glam::vec2(200.0, 100.0)],
            style: StrokeStyle { color: glam::vec4(0.8, 0.8, 0.8, 1.0), width: 1.0, ..StrokeStyle::default() },
        };
        let series = StrokePath {
            points: vec![
                glam::vec2(0.0, 80.0),
                glam::vec2(50.0, 30.0),
                glam::vec2(100.0, 60.0),
                glam::vec2(150.0, 10.0),
                glam::vec2(200.0, 40.0),
            ],
            style: StrokeStyle { color: glam::vec4(0.2, 0.7, 1.0, 1.0), width: 2.0, ..StrokeStyle::default() },
        };
        let dashed = StrokePath {
            points: vec![glam::vec2(0.0, 50.0), glam::vec2(200.0, 50.0)],
            style: StrokeStyle {
                color: glam::vec4(1.0, 1.0, 1.0, 0.5),
                dash_length: 6.0,
                dash_gap: 4.0,
                cap: StrokeCap::Butt,
                ..StrokeStyle::default()
            },
        };

        let _panel_scope = ui.element(&panel);
        let stroked = ui.stroke_paths(&chart, &[axis, series, dashed]);
        let filled = ui.fill_quads(
            &chart,
            &[
                FillQuad {
                    position: glam::vec2(10.0, 10.0),
                    size: glam::vec2(40.0, 20.0),
                    color: glam::vec4(1.0, 0.3, 0.3, 0.9),
                    corner_radius: CornerRadius::all(4.0),
                },
                FillQuad {
                    position: glam::vec2(60.0, 10.0),
                    size: glam::vec2(40.0, 20.0),
                    color: glam::vec4(0.3, 1.0, 0.3, 0.9),
                    corner_radius: CornerRadius::default(),
                },
            ],
        );
        let tau = std::f32::consts::TAU;
        let sectors = ui.fill_sectors(
            &chart,
            &[
                Sector {
                    center: glam::vec2(150.0, 50.0),
                    inner_radius: 20.0,
                    outer_radius: 40.0,
                    start_angle: 0.0,
                    end_angle: tau * 0.6,
                    color: glam::vec4(1.0, 0.8, 0.2, 1.0),
                    ..Sector::default()
                },
                Sector {
                    center: glam::vec2(150.0, 50.0),
                    inner_radius: 20.0,
                    outer_radius: 40.0,
                    start_angle: tau * 0.6,
                    end_angle: tau,
                    color: glam::vec4(0.6, 0.3, 1.0, 1.0),
                    ..Sector::default()
                },
            ],
        );
        let time = start.elapsed().as_secs_f32();
        let push_constants: Vec<u8> = [time, 0.0, 0.0, 0.0].iter().flat_map(|f| f.to_le_bytes()).collect();
        let custom = ui.stroke_custom(
            &chart,
            &[glam::vec2(10.0, 90.0), glam::vec2(100.0, 70.0), glam::vec2(190.0, 90.0)],
            3.0,
            1.0,
            &CustomShader {
                shader_path: "Shaders/ui_stroke_custom_demo.slang".to_string(),
                module_name: "ui_stroke_custom_demo".to_string(),
                fragment_entry_point: None,
                push_constants,
            },
        );

        // A floating, clipped badge anchored to the panel's top-right corner. Clipping makes it
        // visible to `scroll_metrics`, which only reports elements from the last *finished*
        // frame -- so a match proves the floating decl made it through layout.
        {
            let badge = ElementDesc::new()
                .id("ui-ext-float".to_string())
                .width(Sizing::Fixed(80.0))
                .height(Sizing::Fixed(30.0))
                .background(glam::vec4(0.1, 0.1, 0.1, 0.9))
                .clip(Clip { horizontal: false, vertical: true })
                .floating(Some(Floating {
                    element_attach_point: AttachPoint::RightBottom,
                    parent_attach_point: AttachPoint::RightTop,
                    offset: glam::vec2(0.0, -4.0),
                    z_index: 10,
                    ..Floating::new(FloatTarget::Element("ui-ext-panel".to_string()))
                }))
                .z(5);
            let _badge_scope = ui.element(&badge);
        }
        // The chart widget: a two-series line chart with a titled, fixed-range y axis, and a
        // donut pie chart.
        // Charts need a stable id: they size themselves from the previous frame's layout bounds.
        let chart_box = ElementDesc::new()
            .id("ui-ext-line-chart".to_string())
            .width(Sizing::Fixed(260.0))
            .height(Sizing::Fixed(140.0));
        let xs: Vec<f64> = (0..20).map(f64::from).collect();
        let line_drawn = ui.graph(
            &chart_box,
            &GraphDesc {
                graph_type: GraphType::Line,
                y_axis: GraphAxis { min: Some(-1.0), max: Some(1.0), title: "sin/cos".to_string(), ..GraphAxis::default() },
                series: vec![
                    GraphSeries { name: "sin".to_string(), x: xs.clone(), y: xs.iter().map(|x| (x * 0.3).sin()).collect(), ..GraphSeries::default() },
                    GraphSeries {
                        name: "cos".to_string(),
                        x: xs.clone(),
                        y: xs.iter().map(|x| (x * 0.3).cos()).collect(),
                        color: glam::vec4(1.0, 0.5, 0.2, 1.0),
                        marker_radius: 2.0,
                        ..GraphSeries::default()
                    },
                ],
                ..GraphDesc::default()
            },
        );
        let pie_drawn = ui.graph(
            &ElementDesc::new().id("ui-ext-pie-chart".to_string()).width(Sizing::Fixed(140.0)).height(Sizing::Fixed(140.0)),
            &GraphDesc {
                graph_type: GraphType::Pie,
                pie_hole_ratio: 0.5,
                pie_slices: vec![
                    PieSlice { name: "a".to_string(), value: 3.0, color: glam::vec4(0.3, 0.7, 1.0, 1.0) },
                    PieSlice { name: "b".to_string(), value: 1.0, color: glam::vec4(1.0, 0.6, 0.2, 1.0) },
                ],
                ..GraphDesc::default()
            },
        );
        hook_results.graph_line_drawn.store(line_drawn, Ordering::Relaxed);
        hook_results.graph_pie_drawn.store(pie_drawn, Ordering::Relaxed);

        if let Some(metrics) = ui.scroll_metrics("ui-ext-float") {
            if metrics.container_size == glam::vec2(80.0, 30.0) {
                hook_results.floating_laid_out.store(true, Ordering::Relaxed);
            }
        }

        dock.begin_frame(ui, glam::vec4(320.0, 40.0, 400.0, 220.0), 1.0 / 60.0);
        let mut visible = Vec::new();
        for id in ["dock-a", "dock-b", "dock-c"] {
            if let Some(_content) = dock.panel_content(ui, id) {
                visible.push(id.to_string());
                if id == "dock-c" {
                    let probe = ElementDesc::new()
                        .id("ui-ext-dock-probe".to_string())
                        .width(Sizing::Fixed(50.0))
                        .height(Sizing::Fixed(20.0))
                        .clip(Clip { horizontal: false, vertical: true });
                    let _probe = ui.element(&probe);
                }
            }
        }
        let _events = dock.end_frame(ui);
        *hook_results.dock_visible.lock().unwrap() = visible;
        if ui.scroll_metrics("ui-ext-dock-probe").is_some_and(|m| m.container_size == glam::vec2(50.0, 20.0)) {
            hook_results.dock_content_laid_out.store(true, Ordering::Relaxed);
        }

        widget_demo.run(ui);

        hook_results.stroke_paths_ok.store(stroked, Ordering::Relaxed);
        hook_results.fill_quads_ok.store(filled, Ordering::Relaxed);
        hook_results.fill_sectors_ok.store(sectors, Ordering::Relaxed);
        hook_results.stroke_custom_ok.store(custom, Ordering::Relaxed);
        hook_results.frames.fetch_add(1, Ordering::Relaxed);
    });
    eprintln!("[ui-ext] installed UI draw hook (stroke_paths/fill_quads/fill_sectors/stroke_custom)");
    results
}

/// Widget-demo observations, written from inside the UI draw hook and checked from `frame`.
static WIDGET_LOG: std::sync::Mutex<Vec<String>> = std::sync::Mutex::new(Vec::new());
static WIDGET_CHECKBOX_CLICKED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
static WIDGET_BOUNDS_FOUND: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
static WIDGET_MASONRY_ITEMS: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
static WIDGET_SCROLL_CONTENT_RAN: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

static TEXT_TEXTURE_HANDLE: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);

/// State for the widget demo, kept across frames inside the draw hook.
struct WidgetDemo {
    frame: u32,
    button: sturdy::widgets::ButtonState,
    checkbox: sturdy::widgets::ToggleState,
    radio: sturdy::widgets::ToggleState,
    switch: sturdy::widgets::ToggleState,
    slider: sturdy::widgets::SliderState,
    text: sturdy::widgets::TextEditState,
    scroll: sturdy::widgets::ScrollAreaState,
    dropdown: sturdy::widgets::DropdownState,
    picker: sturdy::widgets::ColorPickerState,
    // Widgets don't own your data: the demo keeps each widget's value and applies what they report.
    checked: bool,
    radio_choice: u8,
    radio_b: sturdy::widgets::ToggleState,
    switch_on: bool,
    slider_value: f64,
    picker_color: glam::Vec4,
    dropdown_selected: usize,
}

impl WidgetDemo {
    fn new() -> Self {
        use sturdy::widgets::*;
        Self {
            frame: 0,
            button: ButtonState::new(),
            checkbox: ToggleState::new(),
            radio: ToggleState::new(),
            switch: ToggleState::new(),
            slider: SliderState::new(),
            text: TextEditState::with_text("seed"),
            scroll: ScrollAreaState::new(),
            dropdown: DropdownState::new(),
            picker: ColorPickerState::new(),
            checked: true,
            radio_choice: 0,
            radio_b: ToggleState::new(),
            switch_on: true,
            slider_value: 50.0,
            picker_color: glam::vec4(0.8, 0.2, 0.4, 1.0),
            dropdown_selected: 1,
        }
    }

    fn run(&mut self, ui: &mut sturdy::ui::Ui<'_>) {
        use sturdy::ui::{ElementDesc, Sizing, TextStyle};
        use sturdy::widgets::*;
        let sized = |id: &str, w: f32, h: f32| ElementDesc::new().id(id.to_string()).width(Sizing::Fixed(w)).height(Sizing::Fixed(h));
        let mut log = Vec::new();

        let (button, scope) = ui.button(&sized("w-button", 120.0, 32.0), &mut self.button, &ButtonStyle::default(), true);
        if let Some(scope) = scope {
            ui.text("OK", &TextStyle::default());
            drop(scope);
        }
        log.push(format!("button:hovered={} clicked={}", button.hovered, button.clicked));

        let toggle = ToggleStyle::default();
        let checkbox = ui.checkbox(&sized("w-checkbox", 24.0, 24.0), &mut self.checkbox, &toggle, self.checked, 0, true);
        let radio = ui.radio_button(&sized("w-radio", 24.0, 24.0), &mut self.radio, &toggle, self.radio_choice == 0, true);
        let radio_b = ui.radio_button(&sized("w-radio-b", 24.0, 24.0), &mut self.radio_b, &toggle, self.radio_choice == 1, true);
        let switch = ui.switch(&sized("w-switch", 48.0, 24.0), &mut self.switch, &toggle, self.switch_on, true);
        if checkbox.clicked {
            self.checked = !self.checked;
        }
        if radio.clicked {
            self.radio_choice = 0;
        }
        if radio_b.clicked {
            self.radio_choice = 1;
        }
        if switch.clicked {
            self.switch_on = !self.switch_on;
        }
        log.push(format!("toggles:hovered={}/{}/{}", checkbox.hovered, radio.hovered, switch.hovered));
        if checkbox.clicked {
            WIDGET_CHECKBOX_CLICKED.store(true, std::sync::atomic::Ordering::SeqCst);
        }
        // Aim synthetic pointer input at the checkbox using its laid-out bounds: press on one frame,
        // release on the next.
        match self.frame {
            3 | 4 => {
                if let Some((position, size)) = ui.element_bounds("w-checkbox") {
                    ui.set_pointer_position(position + size * 0.5);
                    ui.set_pointer_down(self.frame == 3);
                    WIDGET_BOUNDS_FOUND.store(true, std::sync::atomic::Ordering::SeqCst);
                }
            }
            5 => ui.clear_pointer_transitions(),
            // Open the dropdown by clicking its trigger (the widget's id), then pick option 2 ("w-dropdown#2").
            8 | 9 => {
                if let Some((position, size)) = ui.element_bounds("w-dropdown") {
                    ui.set_pointer_position(position + size * 0.5);
                    ui.set_pointer_down(self.frame == 8);
                }
            }
            12 | 13 => {
                if let Some((position, size)) = ui.element_bounds("w-dropdown#2") {
                    ui.set_pointer_position(position + size * 0.5);
                    ui.set_pointer_down(self.frame == 12);
                }
            }
            _ => {}
        }

        let keys = if self.frame == 0 { vec![SliderKey::Increment] } else { Vec::new() };
        let slider = ui.slider(
            &sized("w-slider", 200.0, 24.0),
            &SliderConfig::range(0.0, 100.0),
            &SliderStyle::default(),
            &mut self.slider,
            self.slider_value,
            &SliderInput { keys, request_focus: self.frame == 0, request_blur: false },
            true,
        );
        self.slider_value = slider.value;
        log.push(format!("slider:value={} focused={}", slider.value, slider.focused));

        if self.frame == 0 {
            self.text.set_focused(true);
            ui.type_text("abc");
        }
        let field = ui.text_field(&sized("w-text", 200.0, 32.0), &TextInputStyle::default(), &mut self.text, "type here", true);
        log.push(format!("text:{:?} changed={} focused={}", self.text.text(), field.changed, field.focused));
        // Injected input persists until cleared (the engine clears real input each tick itself).
        ui.clear_text_input_transitions();

        let scroll_ran = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
        let ran = scroll_ran.clone();
        ui.scroll_area("w-scroll", &sized("w-scroll", 160.0, 60.0), &ScrollbarStyle::default(), &mut self.scroll, true, move |ui| {
            ui.text("scrolled content", &TextStyle::default());
            ran.store(true, std::sync::atomic::Ordering::SeqCst);
        });
        if scroll_ran.load(std::sync::atomic::Ordering::SeqCst) {
            WIDGET_SCROLL_CONTENT_RAN.store(true, std::sync::atomic::Ordering::SeqCst);
        }

        let dropdown = ui.dropdown(
            "w-dropdown",
            &sized("w-dropdown-trigger", 140.0, 30.0),
            &DropdownStyle::default(),
            &mut self.dropdown,
            self.dropdown_selected,
            &["Low", "Medium", "High"],
            0,
            14,
        );
        if dropdown.changed {
            self.dropdown_selected = dropdown.selected_index;
        }
        log.push(format!("dropdown:selected={} open={}", dropdown.selected_index, self.dropdown.is_open()));

        let picked = ui.color_picker(
            "w-picker",
            &sized("w-picker", 240.0, 260.0),
            &ColorPickerConfig::default(),
            &ColorPickerStyle::default(),
            &mut self.picker,
            self.picker_color,
            None,
            true,
        );
        self.picker_color = picked.color;
        log.push(format!("picker:color={:?} space={:?}", picked.color, picked.color_space));

        let items: Vec<(f32, Box<dyn FnMut(&mut sturdy::ui::Ui<'_>)>)> = (0..5)
            .map(|i| {
                (30.0 + 10.0 * i as f32, Box::new(move |ui: &mut sturdy::ui::Ui<'_>| {
                    ui.text(&format!("item {i}"), &TextStyle::default());
                    WIDGET_MASONRY_ITEMS.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
                }) as Box<dyn FnMut(&mut sturdy::ui::Ui<'_>)>)
            })
            .collect();
        let masonry_ok = ui.masonry(&sized("w-masonry", 200.0, 150.0), &MasonryConfig::default(), items);
        log.push(format!("masonry:ok={masonry_ok}"));

        if self.frame == 0 {
            let original = ui.scroll_settings();
            ui.set_scroll_settings(&ScrollSettings { wheel_multiplier: 7.0, ..original });
            assert_eq!(ui.scroll_settings().wheel_multiplier, 7.0, "scroll settings round-trip");
            ui.set_scroll_settings(&original);
            let style = TextStyle::default();
            assert_eq!(ui.hit_test_text(&style, "hello", 0.0), 0, "left edge hits offset 0");
            assert_eq!(ui.hit_test_text(&style, "hello", 10_000.0), 5, "far right hits the end");
            let mid = ui.hit_test_text(&style, "hello", 20.0);
            assert!((1..=4).contains(&mid), "a point inside the text lands between characters: {mid}");
            let managed = ui.cursor_management_enabled();
            ui.set_cursor_management_enabled(!managed);
            assert_eq!(ui.cursor_management_enabled(), !managed, "cursor management toggles");
            ui.set_cursor_management_enabled(managed);
        }

        // Free-drawn text on the screen, and a text texture drawn as an image.
        ui.draw_text(glam::vec2(16.0, 300.0), "Free-drawn screen text", &TextStyle { font_size: 28, ..TextStyle::default() });
        let handle = TEXT_TEXTURE_HANDLE.load(std::sync::atomic::Ordering::SeqCst);
        if handle != 0 {
            ui.draw_text_target(glam::vec2(16.0, 340.0), glam::vec2(360.0, 48.0), sturdy::assets::RendererTextureHandle::from_raw(handle));
        }

        *WIDGET_LOG.lock().unwrap() = log;
        self.frame += 1;
    }
}

/// True once the widget demo has run enough frames; asserts what it observed.
fn check_widget_demo(frames_waited: u32) -> bool {
    let log = WIDGET_LOG.lock().unwrap().clone();
    if log.is_empty() {
        assert!(frames_waited < 600, "the widget demo never ran");
        return false;
    }
    if !WIDGET_SCROLL_CONTENT_RAN.load(std::sync::atomic::Ordering::SeqCst) || !log.iter().any(|l| l.starts_with("text:\"seedabc\"")) {
        assert!(frames_waited < 600, "widget demo incomplete: {log:?}");
        return false;
    }
    if frames_waited < 30 {
        return false; // let the injected clicks (checkbox at frame 3, dropdown trigger 8, option 12) play out
    }
    assert!(log.iter().any(|l| l.starts_with("dropdown:selected=2")), "clicking the trigger then option 2 selects it: {log:?}");
    assert!(WIDGET_MASONRY_ITEMS.load(std::sync::atomic::Ordering::SeqCst) >= 5, "masonry built its items");
    assert!(log.iter().any(|l| l == "masonry:ok=true"), "masonry laid out: {log:?}");
    let picker = log.iter().find(|l| l.starts_with("picker:")).expect("picker ran");
    assert!(picker.contains("Srgb"), "the picker starts in sRGB: {picker}");

    // Easing curves: every one runs from 0 to 1, and a few have known midpoints.
    use sturdy::widgets::Easing;
    for e in [Easing::Linear, Easing::QuadIn, Easing::CubicInOut, Easing::SineOut, Easing::ExpoInOut, Easing::CircIn,
              Easing::BackOut, Easing::ElasticOut, Easing::BounceOut, Easing::BounceInOut] {
        assert!(e.apply(0.0).abs() < 1e-3 && (e.apply(1.0) - 1.0).abs() < 1e-3, "{e:?} runs 0 -> 1");
    }
    assert!((Easing::Linear.apply(0.5) - 0.5).abs() < 1e-6);
    assert!((Easing::QuadIn.apply(0.5) - 0.25).abs() < 1e-6);
    assert_eq!(sturdy::widgets::ColorPickerSpace::Oklab.name().to_lowercase(), "oklab");
    if !WIDGET_BOUNDS_FOUND.load(std::sync::atomic::Ordering::SeqCst) || log.len() < 5 || frames_waited < 12 {
        assert!(frames_waited < 600, "element_bounds never found the checkbox: {log:?}");
        return false;
    }
    assert!(
        WIDGET_CHECKBOX_CLICKED.load(std::sync::atomic::Ordering::SeqCst),
        "a pointer press+release injected at the checkbox's bounds clicks it"
    );
    eprintln!("[widgets] button/toggles/slider/text field/scroll area/dropdown ran; injected pointer clicked the checkbox; last frame: {log:?}");
    true
}

/// Returns `true` once the UI draw hook has run for a few frames with every primitive accepted.
fn check_ui_draw_demo(results: &UiDemoResults, frames_waited: u32) -> bool {
    use std::sync::atomic::Ordering;
    let frames = results.frames.load(Ordering::Relaxed);
    if frames < 5 {
        assert!(frames_waited < 600, "[ui-ext] UI draw hook ran only {frames} time(s) in {frames_waited} frames");
        return false;
    }
    let stroked = results.stroke_paths_ok.load(Ordering::Relaxed);
    let filled = results.fill_quads_ok.load(Ordering::Relaxed);
    let sectors = results.fill_sectors_ok.load(Ordering::Relaxed);
    let custom = results.stroke_custom_ok.load(Ordering::Relaxed);
    eprintln!(
        "[ui-ext] after {frames} UI frames: stroke_paths={stroked} fill_quads={filled} fill_sectors={sectors} stroke_custom={custom} (expected all true)"
    );
    assert!(stroked && filled && sectors && custom, "every raw UI draw call must be accepted by an open UI session");
    let floating = results.floating_laid_out.load(Ordering::Relaxed);
    eprintln!("[ui-ext] floating badge (attached to the panel by id) laid out at its declared 80x30 = {floating} (expected true)");
    assert!(floating, "the floating element must appear in the finished layout at its declared size");
    let line = results.graph_line_drawn.load(Ordering::Relaxed);
    let pie = results.graph_pie_drawn.load(Ordering::Relaxed);
    eprintln!("[ui-ext] graph widget: line chart drawn={line} donut pie drawn={pie} (expected true, true)");
    assert!(line && pie, "the chart widget must report both charts drawn");
    let adds = results.dock_add_results.lock().unwrap().clone();
    let visible = results.dock_visible.lock().unwrap().clone();
    let content = results.dock_content_laid_out.load(Ordering::Relaxed);
    eprintln!(
        "[ui-ext] docking: add_panel(a, b, c, duplicate a) = {adds:?} (expected [true, true, true, false]); visible panels = {visible:?} (expected [dock-b, dock-c]); content inside dock-c laid out = {content}"
    );
    assert_eq!(adds, vec![true, true, true, false]);
    assert_eq!(visible, vec!["dock-b".to_string(), "dock-c".to_string()], "one visible tab per leaf: b (added last) in the left leaf, c in the split");
    assert!(content, "an element built inside a docked panel's content region must reach the layout");
    eprintln!("[ui-ext] all UI-draw demonstration checks passed");
    true
}

struct Spinner {
    yaw: f32,
    rhi_demo_done: bool,
    native_window_checked: bool,
    window_control_done: bool,
    timings_checked: bool,
    text_renderer: Option<sturdy::text_render::TextRenderer>,
    pipeline_demo_done: bool,
    assets_demo_done: bool,
    schedule_demo_checked: bool,
    schedule_events_checked: bool,
    widgets_checked: bool,
    widgets_frames: u32,
    schedule_events_frames: u32,
    schedule_demo_frames_waited: u32,
    schedule_targets: Option<(Entity, Entity)>,
    window_demo_queued: bool,
    window_demo_checked: bool,
    window_demo_frames_waited: u32,
    device_demo: DeviceDemo,
    ui_demo: Option<Arc<UiDemoResults>>,
    ui_demo_checked: bool,
    ui_demo_frames_waited: u32,
}

/// Demo component: a world-space position. `#[repr(C)]` + `bytemuck::Pod`/`Zeroable` is what makes
/// it eligible for `Component` — no padding-sensitive layout, no pointers.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, bytemuck::Pod, bytemuck::Zeroable)]
struct Position {
    x: f32,
    y: f32,
    z: f32,
}

impl sturdy::Component for Position {
    const NAME: &'static str = "hello.demo.position";
}

/// Demo component: a linear velocity, kept separate from `Position` to exercise multi-component
/// spawns and `World::query::<(Position, Velocity)>()`.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, bytemuck::Pod, bytemuck::Zeroable)]
struct Velocity {
    x: f32,
    y: f32,
    z: f32,
}

impl sturdy::Component for Velocity {
    const NAME: &'static str = "hello.demo.velocity";
}

/// Exercises direct primary-window control.
fn demonstrate_window_control(engine: &mut Engine<'_>) {
    let mut window = engine.primary_window_control().expect("a primary window exists");
    window.set_title("Hello from Rust (window-control demo)").expect("set_title");
    assert!(window.size().is_some_and(|s| s.x > 0 && s.y > 0), "size reads back");
    let clip = format!("sturdy-clipboard-{}", std::process::id());
    window.set_clipboard_text(&clip).expect("set clipboard");
    assert_eq!(window.clipboard_text(), clip, "the clipboard round-trips");
    window.set_minimum_size(glam::uvec2(320, 200)).expect("min size");
    window.set_opacity(0.95).expect("opacity");
    let opacity = window.opacity().expect("read opacity");
    assert!((opacity - 0.95).abs() < 0.05, "opacity reads back: {opacity}");
    window.set_opacity(1.0).ok();
    window.set_cursor_visible(true).expect("cursor visible");
    assert!(!window.close_requested());
    let _ = (window.position(), window.global_cursor_position(), window.hdr_properties(), window.fullscreen_mode());
    window.set_title("Hello from Rust").ok();
    eprintln!("[window-control] title/size/clipboard/opacity/limits checks passed");
}

/// Exercises GPU text rendering: text into an offscreen texture, read back and checked pixel by
/// pixel; re-rendering the same target; span colors; adopting the target as a Renderer texture.
fn demonstrate_text_render(engine: &mut Engine<'_>) -> sturdy::text_render::TextRenderer {
    use sturdy::rhi::Format;
    use sturdy::text::{self, FontStack};
    use sturdy::text_render::{TextFormat, TextItem, TextRenderer};

    let font = text::default_font().expect("default font");
    let fonts = FontStack::new(font);
    let mut renderer = TextRenderer::new(engine).expect("create text renderer");
    let target = renderer.create_target(256, 64, Format::RGBA8Unorm).expect("create text target");
    assert_eq!((target.width, target.height), (256, 64));
    assert!(target.texture().value != 0 && target.view().value != 0 && target.sampler().value != 0);

    let format = TextFormat::new(fonts.clone(), 32.0);
    let measured = format.measure("Hello, text!").expect("measure");
    renderer.draw_text(&target, &[TextItem::new("Hello, text!", glam::vec2(8.0, 8.0), format.clone())]).expect("draw text");
    let pixels = renderer.read_pixels(&target).expect("read pixels back");
    assert_eq!((pixels.width, pixels.height, pixels.bytes_per_pixel), (256, 64, 4));
    assert_eq!(pixels.data.len(), 256 * 64 * 4);

    // Bounding box of the covered pixels (alpha > 32).
    let bounds = |pixels: &sturdy::text_render::Pixels| {
        let (mut min_x, mut min_y, mut max_x, mut max_y, mut count) = (u32::MAX, u32::MAX, 0u32, 0u32, 0u32);
        for y in 0..pixels.height {
            for x in 0..pixels.width {
                if pixels.data[((y * pixels.width + x) * 4 + 3) as usize] > 32 {
                    (min_x, min_y, max_x, max_y, count) = (min_x.min(x), min_y.min(y), max_x.max(x), max_y.max(y), count + 1);
                }
            }
        }
        (min_x, min_y, max_x, max_y, count)
    };
    let (min_x, min_y, max_x, max_y, count) = bounds(&pixels);
    assert!(count > 150, "text covers a good number of pixels: {count}");
    assert!(min_x >= 4 && min_y >= 4, "text starts near the requested origin ({min_x}, {min_y})");
    assert!(max_x <= 8 + measured.x as u32 + 8 && max_y <= 8 + measured.y as u32 + 8, "text stays within its measured box: {max_x},{max_y} vs {measured:?}");
    assert!(max_x > 8 + (measured.x * 0.6) as u32, "the text spans most of its measured width");
    let white = pixels.data.chunks_exact(4).filter(|p| p[3] > 200 && p[0] > 200 && p[1] > 200 && p[2] > 200).count();
    assert!(white > 50, "solid glyph interiors are opaque white: {white}");

    // Re-render the same target with different text and per-span colors.
    let red = glam::vec4(1.0, 0.0, 0.0, 1.0);
    let green = glam::vec4(0.0, 1.0, 0.0, 1.0);
    let colored = TextFormat::new(fonts.clone(), 40.0).span(0..1, red).span(1..2, green);
    renderer.draw_text(&target, &[TextItem::new("WW", glam::vec2(8.0, 4.0), colored)]).expect("draw colored text");
    let second = renderer.read_pixels(&target).expect("read pixels again");
    let (_, _, _, _, count2) = bounds(&second);
    assert!(count2 > 0 && count2 != count, "the target was cleared and redrawn ({count} -> {count2})");
    let reddish = second.data.chunks_exact(4).filter(|p| p[3] > 200 && p[0] > 200 && p[1] < 60 && p[2] < 60).count();
    let greenish = second.data.chunks_exact(4).filter(|p| p[3] > 200 && p[1] > 200 && p[0] < 60 && p[2] < 60).count();
    assert!(reddish > 20 && greenish > 20, "span colors reach the pixels: red {reddish}, green {greenish}");

    // Wrapping: a narrow box wraps to more rows than one line of text.
    let wrapped = TextFormat::new(fonts.clone(), 20.0).wrap(120.0);
    assert!(wrapped.layout("the quick brown fox jumps over the lazy dog").unwrap().lines.len() >= 3);

    // Adopted as a Renderer texture (what `Ui::image` takes).
    let adopted = renderer.adopt(engine, &target).expect("adopt the target as a Renderer texture");
    assert!(adopted.is_valid());
    let resources = engine.assets().rhi_texture(adopted).expect("the adopted texture resolves to RHI resources");
    assert_eq!(resources.size, glam::uvec2(256, 64));
    assert_eq!(resources.view.value, target.view().value, "same RHI view underneath");

    assert!(renderer.destroy_target(target));
    assert!(renderer.read_pixels(&target).is_err(), "a destroyed target is gone");

    // A long-lived target shown on the screen through the UI (see `WidgetDemo::run`).
    let showcase = renderer.create_target(360, 48, Format::RGBA8Unorm).expect("showcase target");
    let banner = TextFormat::new(fonts.clone(), 30.0).color(glam::vec4(1.0, 0.85, 0.2, 1.0));
    renderer.draw_text(&showcase, &[TextItem::new("Text in a texture", glam::vec2(6.0, 4.0), banner)]).expect("draw showcase");
    let handle = renderer.adopt(engine, &showcase).expect("adopt showcase");
    TEXT_TEXTURE_HANDLE.store(handle.raw(), std::sync::atomic::Ordering::SeqCst);

    // Text canvas: text far apart on a huge surface; only the requested tiles are rendered.
    let canvas = renderer.create_canvas(256, 4).expect("create canvas");
    assert!(canvas.tile_size > 0 && canvas.tile_size <= 256);
    let big = TextFormat::new(fonts.clone(), 48.0);
    renderer
        .canvas_draw_text(
            &canvas,
            &[
                TextItem::new("near", glam::vec2(20.0, 30.0), big.clone()),
                TextItem::new("far away", glam::vec2(100_000.0, 250_000.0), big.clone()),
                TextItem::new("negative", glam::vec2(-3_000.0, -40.0), big.clone()),
            ],
        )
        .expect("draw text on the canvas");
    let tile = canvas.tile_size as i32;
    let near = renderer.canvas_tiles(&canvas, glam::ivec2(0, 0), glam::uvec2(300, 300)).expect("tiles near the origin");
    let mut coords: Vec<(i32, i32)> = near.iter().map(|t| (t.coord.x, t.coord.y)).collect();
    coords.sort();
    let expected: Vec<(i32, i32)> = (0..=(299 / tile)).flat_map(|x| (0..=(299 / tile)).map(move |y| (x, y))).collect();
    assert_eq!(coords, expected, "a 300x300 viewport covers exactly the tiles it overlaps");
    for t in &near {
        assert!(t.view.value != 0);
        assert_eq!(t.logical_position, t.coord * tile);
        assert_eq!(t.logical_size, glam::UVec2::splat(canvas.tile_size));
    }
    let far = renderer.canvas_tiles(&canvas, glam::ivec2(100_000, 250_000), glam::uvec2(200, 100)).expect("tiles far away");
    assert!(!far.is_empty() && far.iter().all(|t| t.logical_position.x <= 100_200 && t.logical_position.y <= 250_100));
    let negative = renderer.canvas_tiles(&canvas, glam::ivec2(-3_000, -40), glam::uvec2(300, 100)).expect("tiles at negative coordinates");
    assert!(negative.iter().any(|t| t.coord.x < 0), "the canvas extends into negative coordinates");
    // More tiles than the residency budget: every requested tile is still returned.
    let many = renderer.canvas_tiles(&canvas, glam::ivec2(0, 0), glam::uvec2(canvas.tile_size * 4, canvas.tile_size * 4)).expect("a large viewport");
    assert_eq!(many.len(), 16);
    renderer.canvas_draw_text(&canvas, &[TextItem::new("late addition", glam::vec2(40.0, 200.0), big)]).expect("draw again after tiles are resident");
    assert!(renderer.canvas_tiles(&canvas, glam::ivec2(0, 0), glam::uvec2(300, 300)).is_ok(), "dirty tiles re-render");
    assert!(renderer.canvas_sampler().is_some());
    assert!(renderer.destroy_canvas(canvas) && !renderer.destroy_canvas(canvas));
    assert!(renderer.canvas_tiles(&canvas, glam::ivec2(0, 0), glam::uvec2(10, 10)).is_err(), "a destroyed canvas is gone");

    // Text in the world: baked to a texture asset on a self-lit quad, spawned into the scene.
    let label = sturdy::text_render::world_label(
        engine,
        &mut renderer,
        &[TextItem::new("World text", glam::vec2(8.0, 8.0), TextFormat::new(fonts.clone(), 96.0).color(glam::vec4(0.3, 0.9, 1.0, 1.0)))],
        640,
        112,
        0.6,
        "Shaders/gbuffer_geometry.slang",
        "hello world label",
    )
    .expect("create a world label");
    assert!(label.model.is_valid() && label.texture.is_valid());
    assert!((label.size_meters.x / label.size_meters.y - 640.0 / 112.0).abs() < 1e-3);
    let mut world = engine.ecs();
    let entity = world
        .spawn_builder()
        .with(sturdy::scene::WorldTransform::from_translation([-1.6, 0.9, 0.0]))
        .with(sturdy::scene::ModelRenderer::new(label.model))
        .build()
        .expect("spawn the world label");
    assert!(world.is_alive(entity));
    eprintln!("[text-render] offscreen target render / readback / re-render / spans / adopt / world label checks passed");
    renderer
}

/// Exercises the device-free RHI helpers.
fn demonstrate_rhi_util() {
    use sturdy::rhi::Format;
    use sturdy::rhi_util::{self, FormatExt, DeviceType, PowerPreference, PresentMode, PresentStrategy, RenderThreadingMode};

    assert!(Format::BC7Unorm.is_block_compressed() && !Format::RGBA8Unorm.is_block_compressed());
    assert!(Format::D32Float.has_depth() && !Format::D32Float.has_stencil() && Format::D32Float.is_depth_stencil());
    assert!(Format::D24UnormS8Uint.has_depth() && Format::D24UnormS8Uint.has_stencil());
    assert!(!Format::RGBA8Unorm.is_depth_stencil());

    let names = sturdy::rhi::all_feature_names();
    let (a, b, c, d) = (names[0].as_str(), names[1].as_str(), names[2].as_str(), names[3].as_str());
    let negotiation = rhi_util::negotiate_features(&[a, b], &[a, c], &[b, d]);
    assert_eq!(negotiation.enabled_required, vec![a.to_string()]);
    assert_eq!(negotiation.missing_required, vec![c.to_string()]);
    assert_eq!(negotiation.enabled_optional, vec![b.to_string()]);
    assert_eq!(negotiation.unavailable_optional, vec![d.to_string()]);
    assert!(!negotiation.required_satisfied() && !negotiation.optional_fully_enabled());
    assert!(rhi_util::negotiate_features(&[a], &[a], &[]).required_satisfied());

    assert!(rhi_util::score_adapter(DeviceType::Discrete, PowerPreference::HighPerformance) > rhi_util::score_adapter(DeviceType::Integrated, PowerPreference::HighPerformance));
    assert!(rhi_util::score_adapter(DeviceType::Integrated, PowerPreference::LowPower) > rhi_util::score_adapter(DeviceType::Discrete, PowerPreference::LowPower));
    assert!(rhi_util::name_contains("AMD Radeon RX 9070", "radeon") && !rhi_util::name_contains("AMD Radeon", "nvidia"));

    assert_eq!(rhi_util::choose_render_threading_mode(true, false, true), RenderThreadingMode::DedicatedRenderThread);
    assert_eq!(rhi_util::choose_render_threading_mode(true, false, false), RenderThreadingMode::SingleThreaded);
    assert_eq!(PresentStrategy::TearFreeOrdered.mode_preference(), [PresentMode::Fifo; 4]);
    assert_eq!(PresentStrategy::Unsynchronized.choose_mode(&[PresentMode::Fifo, PresentMode::Immediate]), PresentMode::Immediate);
    assert_eq!(PresentStrategy::Unsynchronized.choose_mode(&[PresentMode::Fifo]), PresentMode::Fifo, "falls back to what exists");
    assert_eq!(PresentMode::Mailbox.name(), "Mailbox");
    assert_eq!(PresentStrategy::VariableRefresh.name(), "VariableRefresh");
    assert_eq!(rhi_util::transparent_composite_alpha_preference()[0], "Premultiplied");
    eprintln!("[rhi-util] format / negotiation / scoring / threading / present-mode checks passed");
}

/// Exercises the full Slang compiler API: targets, macros, reflection, variants.
fn demonstrate_shader_compiler_full() {
    use sturdy::shader_compiler::{self as sc, CompileOptions, Source, Stage, TargetFormat, VariantCache};

    let source = Source::text(
        "full_demo",
        r#"
        struct Params { float4 tint; float scale; };
        [[vk::binding(0, 0)]] ConstantBuffer<Params> params;
        [[vk::binding(1, 0)]] Texture2D<float4> image;
        [[vk::binding(2, 0)]] SamplerState linear_sampler;

        struct VsOut { float4 position : SV_Position; float2 uv : TEXCOORD0; };

        [shader("vertex")]
        VsOut vertexMain(uint id : SV_VertexID) {
            VsOut o;
            o.uv = float2((id << 1) & 2, id & 2);
            o.position = float4(o.uv * 2.0 - 1.0, 0.0, 1.0);
            return o;
        }

        [shader("fragment")]
        float4 fragmentMain(VsOut i) : SV_Target {
        #ifdef INVERT
            return 1.0 - image.Sample(linear_sampler, i.uv) * params.tint * params.scale;
        #else
            return image.Sample(linear_sampler, i.uv) * params.tint * params.scale;
        #endif
        }

        [shader("compute")]
        [numthreads(8, 4, 1)]
        void computeMain(uint3 tid : SV_DispatchThreadID) {}
        "#,
    );
    let options = CompileOptions::default()
        .entry("vertexMain", Stage::Vertex)
        .entry("fragmentMain", Stage::Fragment)
        .entry("computeMain", Stage::Compute);

    // Reflection without codegen: entry points, stages, thread groups, descriptor layout.
    let reflected = sc::reflect(&source, &options).expect("reflect");
    assert!(reflected.code.is_empty(), "reflect() generates no code");
    assert!(reflected.reflection.global_parameters.iter().any(|p| p.name == "params"), "reflect() reports globals");
    eprintln!("[shader-compiler] reflect-only entry points: {:?}", reflected.reflection.entry_points.iter().map(|e| e.name.as_str()).collect::<Vec<_>>());
    let reflected = sc::compile(&source, &options).expect("compile for entry-point reflection");
    let names: Vec<&str> = reflected.reflection.entry_points.iter().map(|e| e.name.as_str()).collect();
    assert!(names.contains(&"vertexMain") && names.contains(&"fragmentMain") && names.contains(&"computeMain"), "{names:?}");
    let compute = reflected.reflection.entry_points.iter().find(|e| e.name == "computeMain").unwrap();
    assert_eq!(compute.stage, Stage::Compute);
    assert_eq!(compute.thread_group_size, [8, 4, 1]);
    let params = reflected.reflection.global_parameters.iter().find(|p| p.name == "params").expect("params global");
    assert_eq!((params.binding, params.binding_space), (0, 0));
    assert_eq!(params.size, 32, "a float4 + float constant buffer is 32 bytes");
    assert_eq!(params.type_kind, 6, "ConstantBuffer kind");
    assert!(reflected.reflection.json.contains("\"tint\"") && reflected.reflection.json.contains("\"scale\""), "struct member layout is in the reflection JSON");
    assert!(reflected.reflection.global_parameters.iter().any(|p| p.name == "image"));
    assert!(!reflected.reflection.descriptor_sets.is_empty(), "descriptor sets are reported");
    assert!(reflected.reflection.json.contains("params"), "the reflection JSON is included");
    let vertex = reflected.reflection.entry_points.iter().find(|e| e.name == "vertexMain").unwrap();
    assert!(vertex.results.iter().any(|r| r.name == "position" || r.semantic_name.eq_ignore_ascii_case("SV_Position") || !r.fields.is_empty()));

    // Codegen for several targets at once.
    let multi = sc::compile(&source, &options.clone().also_target(TargetFormat::Hlsl).also_target(TargetFormat::Glsl)).expect("multi-target compile");
    let spirv = multi.code_for("fragmentMain", TargetFormat::Spirv).expect("SPIR-V for the fragment shader");
    assert_eq!(&spirv.bytes[..4], &0x0723_0203u32.to_le_bytes(), "SPIR-V magic number");
    let hlsl = multi.code_for("fragmentMain", TargetFormat::Hlsl).expect("HLSL for the fragment shader");
    assert!(String::from_utf8_lossy(&hlsl.bytes).contains("fragmentMain"), "HLSL is source text");
    // WGSL needs a session of its own (the engine refuses to mix it with other targets).
    let wgsl_only = sc::compile(&source, &options.clone().target(TargetFormat::Wgsl));
    assert!(wgsl_only.as_ref().is_ok_and(|c| c.code.iter().any(|x| String::from_utf8_lossy(&x.bytes).contains("fn "))), "WGSL compiles alone: {wgsl_only:?}");
    assert!(sc::compile(&source, &options.clone().also_target(TargetFormat::Wgsl)).is_err(), "WGSL alongside another target is rejected by the engine");
    assert!(multi.code_for("computeMain", TargetFormat::Glsl).is_some());
    assert_eq!(multi.code.len(), 9, "3 entry points x 3 targets");

    // Macros change the generated code; variants are cached per macro set.
    let plain = sc::compile(&source, &options).unwrap();
    let inverted = sc::compile(&source, &options.clone().define("INVERT", "1")).unwrap();
    assert_ne!(plain.code_for("fragmentMain", TargetFormat::Spirv).unwrap().bytes, inverted.code_for("fragmentMain", TargetFormat::Spirv).unwrap().bytes, "INVERT changes the fragment shader");
    let mut cache = VariantCache::new(&source, &options, false);
    assert!(cache.is_empty() && !cache.contains(&[("INVERT", "1")]));
    let first = cache.get(&[("INVERT", "1")]).expect("variant");
    assert_eq!(cache.len(), 1);
    let again = cache.get(&[("INVERT", "1")]).expect("cached variant");
    assert_eq!(cache.len(), 1, "the second request is a cache hit");
    assert_eq!(first.code, again.code);
    cache.get(&[]).expect("base variant");
    assert_eq!(cache.len(), 2);
    cache.invalidate();
    assert!(cache.is_empty());
    assert_eq!(sc::variant_key(&[("A", "1"), ("B", "2")]), sc::variant_key(&[("B", "2"), ("A", "1")]), "keys ignore order");
    assert_ne!(sc::variant_key_hash(&[("A", "1")]), sc::variant_key_hash(&[("A", "2")]));

    // Errors carry diagnostics; search paths let `import` find modules.
    let broken = sc::compile(&Source::text("broken", "float4 f() { return undefined_symbol; }"), &CompileOptions::default().entry("f", Stage::Fragment));
    assert!(broken.is_err_and(|e| e.0.contains("undefined_symbol")), "the diagnostic names the bad symbol");
    let engine_shader = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("assets/tint.slang");
    let file = sc::compile(&Source::file(engine_shader.to_string_lossy()), &CompileOptions::default().entry("fragmentMain", Stage::Fragment).search_path(concat!(env!("CARGO_MANIFEST_DIR"), "/../../sturdy-sys/vendor/sturdyengine5/Shaders")));
    eprintln!("[shader-compiler] file compile with `import sturdy_common`: {}", file.as_ref().map(|_| "ok".to_string()).unwrap_or_else(|e| e.0.lines().next().unwrap_or("").to_string()));
    eprintln!("[shader-compiler] reflection / multi-target codegen / macros / variant cache checks passed");
}

/// Exercises `sturdy::color` (conversion, interpolation, primaries matrices, HDR transfer).
fn demonstrate_color() {
    use sturdy::color::{self, Color, GamutMapping, Primaries, Space, TransferFunction};
    let close = |a: f64, b: f64, eps: f64, what: &str| assert!((a - b).abs() < eps, "{what}: {a} vs {b}");

    // Round trips through every space return to the start.
    let start = Color::srgb(0.8, 0.35, 0.1, 0.9);
    for space in [Space::Linear, Space::Xyz, Space::AdobeRgb, Space::DisplayP3, Space::Rec2020, Space::Hsl, Space::Hsv,
                  Space::Hwb, Space::Lab, Space::Lch, Space::Luv, Space::Oklab, Space::Oklch] {
        let there = start.convert(space);
        assert_eq!(there.space, space);
        if space == Space::DisplayP3 {
            continue; // the engine's DisplayP3 <-> linear matrices don't invert each other (engine plan)
        }
        let back = there.convert(Space::Srgb);
        // Oklab/Oklch round-trip only to ~2e-2 in the engine (see the engine plan).
        let tolerance = if matches!(space, Space::Oklab | Space::Oklch) { 2e-2 } else { 1e-4 };
        for i in 0..4 {
            close(back.components[i], start.components[i], tolerance, &format!("{space:?} round trip component {i}"));
        }
    }
    // Known values: sRGB mid-gray is ~0.2140 linear, white has Oklab lightness 1, HSL of pure red.
    close(Color::srgb(0.5, 0.5, 0.5, 1.0).convert(Space::Linear).components[0], 0.214, 0.001, "sRGB 0.5 -> linear");
    close(Color::srgb(1.0, 1.0, 1.0, 1.0).convert(Space::Oklab).components[0], 1.0, 1e-3, "white Oklab L");
    let red_hsl = Color::srgb(1.0, 0.0, 0.0, 1.0).convert(Space::Hsl).components;
    close(red_hsl[0], 0.0, 1e-6, "red hue");
    close(red_hsl[1], 1.0, 1e-6, "red saturation");
    close(color::srgb_to_linear(color::linear_to_srgb(0.3)), 0.3, 1e-9, "channel transfer round trip");

    // Interpolation and compositing.
    let black = Color::srgb(0.0, 0.0, 0.0, 1.0);
    let white = Color::srgb(1.0, 1.0, 1.0, 1.0);
    close(black.lerp(&white, 0.5).components[0], 0.5, 1e-9, "sRGB lerp midpoint");
    let over = Color::srgb(1.0, 0.0, 0.0, 0.5).over(&Color::srgb(0.0, 0.0, 1.0, 1.0));
    assert!(over.components[0] > 0.3 && over.components[2] > 0.3 && over.alpha() > 0.99, "half red over opaque blue: {over:?}");
    let tinted = Color::srgb(1.0, 1.0, 1.0, 1.0).modulate(&Color::srgb(0.5, 0.25, 1.0, 1.0));
    close(tinted.components[1], 0.25, 1e-9, "modulate");
    close(start.with_alpha(0.25).alpha(), 0.25, 1e-12, "with_alpha");
    close(Color::srgb(2.0, -1.0, 0.5, 1.0).clamp_rgb(0.0, 1.0).components[0], 1.0, 1e-12, "clamp_rgb");
    let pre = Color::srgb(1.0, 0.5, 0.0, 0.5).premultiplied();
    close(pre.components[0], 0.5, 1e-9, "premultiply");
    close(pre.unpremultiplied().components[0], 1.0, 1e-9, "unpremultiply");

    // Primaries, gamut and matrices.
    let srgb = Primaries::from_h273(1).expect("BT.709 primaries");
    assert_eq!(srgb, Primaries::working_space());
    let bt2020 = Primaries::from_h273(9).expect("BT.2020 primaries");
    assert!(Primaries::from_h273(200).is_none());
    let to_xyz = srgb.rgb_to_xyz();
    let white_xyz = to_xyz * glam::Vec3::ONE;
    close(white_xyz.y as f64, 1.0, 1e-3, "white luminance Y");
    let round = srgb.xyz_to_rgb() * to_xyz;
    assert!(round.abs_diff_eq(glam::Mat3::IDENTITY, 1e-4), "xyz<->rgb matrices invert each other");
    let bt709_to_2020 = srgb.conversion_to(&bt2020);
    let pure_green = bt709_to_2020 * glam::Vec3::Y;
    assert!(pure_green.x > 0.0 && pure_green.y < 1.0, "pure BT.709 green is inside the wider BT.2020 gamut");
    assert!(color::chromatic_adaptation(srgb.white, bt2020.white).abs_diff_eq(glam::Mat3::IDENTITY, 1e-3), "same white point: no adaptation");
    let out_of_gamut = glam::vec3(1.4, -0.2, 0.3);
    let clipped = color::map_into_gamut(out_of_gamut, GamutMapping::Clip, 1.0);
    assert!(clipped.min_element() >= 0.0 && clipped.max_element() <= 1.0);
    let desat = color::map_into_gamut(out_of_gamut, GamutMapping::Desaturate, 1.0);
    assert!(desat.min_element() >= -1e-4 && desat.max_element() <= 1.0001 && desat != clipped);

    // Transfer functions.
    assert_eq!(TransferFunction::from_h273(16), TransferFunction::Pq);
    assert_eq!(TransferFunction::from_h273(18), TransferFunction::Hlg);
    assert_eq!(TransferFunction::from_h273(13), TransferFunction::Srgb);
    assert_eq!(TransferFunction::from_h273(99), TransferFunction::Unsupported);
    assert!((color::pq_to_linear(1.0) - 100.0).abs() < 0.1 && color::pq_to_linear(0.0).abs() < 1e-6, "10000 nits is 100x the 100-nit reference white");
    assert!(color::pq_to_linear(0.5) < 1.0 && color::pq_to_linear(0.5) > 0.5, "PQ 0.5 is ~92 nits, just under reference white");
    assert!((color::hlg_to_linear(1.0) - 1.0).abs() < 1e-3);
    assert_eq!(color::half_to_f32(color::f32_to_half(1.5)), 1.5);
    assert_eq!(color::f32_to_half(1.0), 0x3C00);
    eprintln!("[color] conversion / interpolation / compositing / primaries / transfer checks passed");
}

/// Exercises `sturdy::text` (CPU side): fonts, shaping, layout, breaking, outlines, rasterization.
fn demonstrate_text() {
    use sturdy::text::{self, Alignment, FontStack, LayoutOptions, RasterFormat, RasterParams, Segment, ShapeOptions};

    let font = text::default_font().expect("embedded default font loads");
    assert_eq!(text::default_font().unwrap(), font, "the default font id is stable");
    let metrics = font.metrics().expect("metrics");
    assert!(metrics.units_per_em > 0 && metrics.ascender > 0 && metrics.descender <= 0);
    assert!(!font.features("", "").is_empty(), "the default font advertises OpenType features");
    let stack = FontStack::new(font);

    // Shaping and layout.
    let glyphs = text::shape(&stack, "Hello", &ShapeOptions::default()).expect("shape");
    assert_eq!(glyphs.len(), 5);
    assert!(glyphs.iter().all(|g| g.x_advance > 0.0 && !g.is_color));
    let single = text::layout(&stack, "Hello world", &LayoutOptions::default()).expect("layout");
    assert_eq!(single.lines.len(), 1);
    assert!(single.width_em > 3.0 && single.height_em > 0.5, "width {} height {}", single.width_em, single.height_em);
    let wrapped = text::layout(&stack, "Hello world of wrapped text", &LayoutOptions { max_width_em: Some(single.width_em * 0.6), ..LayoutOptions::default() })
        .expect("wrapped layout");
    assert!(wrapped.lines.len() >= 3, "narrow max width wraps: {} lines", wrapped.lines.len());
    assert!(wrapped.width_em <= single.width_em * 0.6 + 0.01, "no wrapped line exceeds the limit");
    assert!(wrapped.height_em > single.height_em, "more lines are taller");
    let newline = text::layout(&stack, "a\nb", &LayoutOptions::default()).expect("newline layout");
    assert_eq!(newline.lines.len(), 2);
    assert!(newline.lines[0].mandatory_break_after);
    let centered = text::layout(&stack, "ab\nabcdefgh", &LayoutOptions::default().alignment(Alignment::Center)).expect("centered layout");
    assert!(centered.lines[0].offset_em > 0.0 && centered.lines[1].offset_em.abs() < 1e-4, "the short line is shifted to the center");
    assert_eq!(single.glyph_positions().len(), "Hello world".chars().count());
    let positions = single.glyph_positions();
    assert!(positions.windows(2).all(|w| w[1].1.x >= w[0].1.x), "glyph pen positions advance left to right");

    // Features: ligatures change the glyph count for "fi" in fonts that have them, and a bad spec is rejected.
    text::validate_features("liga=0, smcp, ss01").expect("valid feature spec");
    assert!(text::validate_features("not a feature spec!!").is_err());
    let with_lig = text::shape(&stack, "office", &ShapeOptions::default()).unwrap().len();
    let without_lig = text::shape(&stack, "office", &ShapeOptions { features: "liga=0, clig=0, calt=0".into(), ..ShapeOptions::default() }).unwrap().len();
    assert!(with_lig <= without_lig, "disabling ligatures never yields fewer glyphs");
    assert!(text::shape(&stack, "x", &ShapeOptions { features: "not a feature spec!!".into(), ..ShapeOptions::default() }).is_err(), "shape reports a bad feature spec");

    // Breaking.
    let breaks = text::line_break_opportunities("hello world\nnext", "");
    assert!(breaks.iter().any(|b| b.byte_index == 6 && !b.mandatory) && breaks.iter().any(|b| b.mandatory));
    assert_eq!(text::grapheme_boundaries("añb", ""), vec![0, 1, 3, 4]);
    assert!(text::word_boundaries("hello world", "").contains(&5));
    assert!(text::is_emoji('😀') && !text::is_emoji('a'));

    // Outlines and rasterization of the glyph for 'H'.
    let h = glyphs_for(&stack, "H")[0];
    let outline = text::glyph_outline(font, h).expect("outline");
    assert!(!outline.contours.is_empty() && outline.contours.iter().flatten().any(|s| matches!(s, Segment::LineTo(_))));
    let (min, max) = text::glyph_bounds(font, h).expect("H has bounds");
    assert!(max.x > min.x && max.y > min.y);
    assert!(text::glyph_bounds(font, glyphs_for(&stack, " ")[0]).is_none(), "a space has no outline");
    let raster = text::rasterize_glyph(font, h, &RasterParams { width: 48, height: 48, scale: 48.0 / metrics.units_per_em as f32, ..RasterParams::default() })
        .expect("rasterize sdf");
    assert_eq!((raster.width, raster.height), (48, 48));
    assert_eq!(raster.pixels.len(), (raster.width * raster.height * raster.channel_count) as usize);
    assert!(raster.pixels.iter().any(|&p| p > 200) && raster.pixels.iter().any(|&p| p < 60), "an SDF has both inside and outside values");
    let msdf = text::rasterize_glyph(font, h, &RasterParams { format: RasterFormat::Msdf, width: 48, height: 48, scale: 48.0 / metrics.units_per_em as f32, ..RasterParams::default() })
        .expect("rasterize msdf");
    assert_eq!(msdf.channel_count, 3);
    assert_eq!(text::color_glyph_format(font, h), text::ColorGlyphFormat::None);

    // Fonts from memory/file and the installed-font database.
    assert!(text::load_font_bytes(b"not a font", 0).is_err());
    assert!(text::load_font_file("/no/such/font.ttf", 0).is_err());
    let maple = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../Fonts/MapleMono-NF-Regular.ttf");
    if maple.exists() {
        let loaded = text::load_font_file(&maple, 0).expect("load Maple Mono");
        assert!(loaded != font && loaded.metrics().is_some());
        let mono = text::layout(&FontStack::new(loaded), "iiii", &LayoutOptions::default()).unwrap();
        let wide = text::layout(&FontStack::new(loaded), "WWWW", &LayoutOptions::default()).unwrap();
        assert!((mono.width_em - wide.width_em).abs() < 1e-3, "a monospace font gives equal widths");
        let database = text::FontDatabase::new(&[maple.parent().unwrap().to_path_buf()]);
        let faces = database.faces();
        assert!(!faces.is_empty(), "the database indexes the Fonts directory");
        assert!(text::unload_font(loaded) && loaded.metrics().is_none() && !text::unload_font(loaded));
    }
    // Documents: an editable rope with undo/redo, position mapping, navigation and search.
    let mut document = text::Document::new("fn main() {\n    let x = (1 + 2);\n}\n");
    assert_eq!(document.line_count(), 4, "three lines plus the empty one after the final newline");
    assert_eq!(document.line_range(1).map(|r| document.slice(r)), Some("    let x = (1 + 2);".to_string()));
    assert_eq!(document.detect_indent(), (text::IndentKind::Spaces, 4));
    assert_eq!(document.line_indent(1).0, 4);
    assert_eq!(document.detect_line_ending(), text::LineEnding::Lf);
    let at_x = document.find("x", 0, true, true).expect("find x");
    assert_eq!(document.offset_to_point(at_x.start), Some(text::Point { line: 1, column: 8 }));
    assert_eq!(document.point_to_offset(text::Point { line: 1, column: 8 }), Some(at_x.start));
    assert_eq!(document.find_all("LET", false).len(), 1);
    assert_eq!(document.word_range_at(at_x.start + 1).map(|r| document.slice(r)), Some("x".into()));
    let open = document.find("(1", 0, true, true).unwrap().start;
    assert_eq!(document.matching_bracket(open).map(|c| document.slice(text::ByteRange::new(c, c + 1))), Some(")".into()));
    let revision = document.revision();
    let changes = document.replace(at_x, "value").expect("edit");
    assert_eq!(changes.changes.len(), 1);
    assert!(document.text().contains("let value = ") && document.revision() > revision);
    assert!(document.replace(text::ByteRange::new(0, 9999), "").is_err(), "an out-of-range edit is rejected");
    document.undo().expect("undo");
    assert!(document.text().contains("let x = ") && !document.text().contains("value"));
    document.redo().expect("redo");
    assert!(document.text().contains("value"));
    let multi = document.apply(&[text::Edit::insert(0, "// "), text::Edit::delete(text::ByteRange::new(3, 5))], text::EditKind::Standalone);
    assert!(multi.is_ok(), "several non-overlapping edits form one transaction: {multi:?}");
    assert_eq!(document.stats().bytes, document.text().len());
    let emoji = text::Document::new("a😀b");
    assert_eq!(emoji.stats().scalars, 3);
    assert_eq!(emoji.offset_to_utf16(5).map(|p| p.column), Some(3), "the emoji is two UTF-16 code units");
    assert_eq!(emoji.clip_offset(3, false), 1, "offsets snap to character boundaries");

    eprintln!("[text] fonts / shaping / layout / breaking / outlines / rasterization checks passed ({} system font dirs)", text::font_search_directories().len());

    fn glyphs_for(stack: &FontStack, s: &str) -> Vec<u32> {
        text::shape(stack, s, &ShapeOptions::default()).unwrap().into_iter().map(|g| g.glyph_id).collect()
    }
}

/// Exercises `sturdy::images` (decode, mip chain, BC compression). All pure CPU functions, so
/// unlike the GPU-backed demos this one runs from `init`.
fn demonstrate_images() {
    use sturdy::images::{self, BcCodec, DecodeOptions, ImageError};

    fn crc32(data: &[u8]) -> u32 {
        let mut crc = !0u32;
        for &b in data {
            crc ^= b as u32;
            for _ in 0..8 {
                crc = if crc & 1 != 0 { (crc >> 1) ^ 0xEDB8_8320 } else { crc >> 1 };
            }
        }
        !crc
    }
    fn chunk(out: &mut Vec<u8>, kind: &[u8; 4], body: &[u8]) {
        out.extend((body.len() as u32).to_be_bytes());
        let mut crc_input = kind.to_vec();
        crc_input.extend(body);
        out.extend(&crc_input);
        out.extend(crc32(&crc_input).to_be_bytes());
    }
    /// Minimal RGBA8 PNG using stored (uncompressed) deflate blocks.
    fn encode_png(rgba: &[u8], w: u32, h: u32) -> Vec<u8> {
        let mut raw = Vec::new();
        for row in rgba.chunks((w * 4) as usize) {
            raw.push(0);
            raw.extend(row);
        }
        let mut z = vec![0x78, 0x01];
        let mut blocks = raw.chunks(65535).peekable();
        while let Some(block) = blocks.next() {
            z.push(blocks.peek().is_none() as u8);
            z.extend((block.len() as u16).to_le_bytes());
            z.extend((!(block.len() as u16)).to_le_bytes());
            z.extend(block);
        }
        let (mut a, mut b) = (1u32, 0u32);
        for &byte in &raw {
            a = (a + byte as u32) % 65521;
            b = (b + a) % 65521;
        }
        z.extend(((b << 16) | a).to_be_bytes());
        let mut png = vec![0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A];
        let mut ihdr = Vec::new();
        ihdr.extend(w.to_be_bytes());
        ihdr.extend(h.to_be_bytes());
        ihdr.extend([8, 6, 0, 0, 0]);
        chunk(&mut png, b"IHDR", &ihdr);
        chunk(&mut png, b"IDAT", &z);
        chunk(&mut png, b"IEND", &[]);
        png
    }

    let (w, h) = (8u32, 8u32);
    let rgba: Vec<u8> = (0..w * h)
        .flat_map(|i| [(i * 4) as u8, (255 - i * 4) as u8, 128, 255])
        .collect();

    let decoded = images::decode(&encode_png(&rgba, w, h), DecodeOptions::default(), "demo.png")
        .expect("decode generated PNG");
    assert_eq!((decoded.width, decoded.height), (w, h));
    assert_eq!(decoded.frames.len(), 1);
    assert_eq!(decoded.pixels().len(), (w * h * 4) as usize);
    // The PNG has no color chunks, so it is assumed sRGB and passes through unchanged.
    assert_eq!(decoded.pixels(), &rgba[..], "8-bit sRGB PNG round-trips through decode");
    assert!(matches!(images::decode(b"not an image", DecodeOptions::default(), "junk"), Err(ImageError::Decode(_))));

    assert_eq!(images::mip_level_count(w, h), 4);
    let chain = images::generate_rgba8_mip_chain(&rgba, w, h, true).expect("mip chain");
    assert_eq!(chain.mip_levels, 4);
    assert_eq!(chain.data.len(), (64 + 16 + 4 + 1) * 4);
    assert_eq!(&chain.data[..rgba.len()], &rgba[..], "level 0 is the source image");
    assert_eq!(images::generate_rgba8_mip_chain(&rgba[..8], w, h, true).err(), Some(ImageError::InvalidInput));

    let bc7 = images::compress(BcCodec::Bc7 { srgb: true }, &rgba, w, h).expect("bc7");
    assert_eq!(bc7.len(), 4 * 16, "8x8 BC7 = four 16-byte blocks");
    let bc1 = images::compress(BcCodec::Bc1 { srgb: true }, &rgba, w, h).expect("bc1");
    assert_eq!(bc1.len(), 4 * 8, "8x8 BC1 = four 8-byte blocks");
    let bc5 = images::compress(BcCodec::Bc5 { channel0: 0, channel1: 1 }, &rgba, w, h).expect("bc5");
    assert_eq!(bc5.len(), 4 * 16);
    let bc4 = images::compress(BcCodec::Bc4 { channel: 0 }, &rgba, w, h).expect("bc4");
    assert_eq!(bc4.len(), 4 * 8);
    let bc7_chain = images::compress_mip_chain(BcCodec::Bc7 { srgb: true }, &chain.data, w, h, chain.mip_levels)
        .expect("bc7 mip chain");
    assert!(!bc7_chain.is_empty() && bc7_chain.len() % 16 == 0 && bc7_chain.len() > bc7.len());
    let gdeflate = images::compress_gdeflate_sibling(&bc7, w, h, true).expect("gdeflate-compress BC7 blocks");
    assert!(!gdeflate.is_empty());
    assert_eq!(images::compress_gdeflate_sibling(&[], w, h, true).err(), Some(ImageError::InvalidInput));
    let payload: Vec<u8> = (0..4096u32).map(|i| (i % 7) as u8).collect();
    let packed = images::gdeflate_compress(&payload, 9).expect("gdeflate compress");
    assert!(packed.len() < payload.len(), "repetitive data compresses");
    assert_eq!(images::gdeflate_decompress(&packed, payload.len()).expect("gdeflate decompress"), payload, "gdeflate round trip");
    let this_file = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("Cargo.toml");
    assert_eq!(images::read_file_accelerated(&this_file), std::fs::read(&this_file).ok(), "accelerated file read matches std");
    assert!(images::read_file_accelerated("/no/such/file").is_none());
    let licenses = images::third_party_licenses();
    assert!(!licenses.is_empty() && licenses.iter().all(|l| !l.project.is_empty() && !l.text.is_empty()), "embedded license texts");
    assert_eq!(images::compress(BcCodec::Bc7 { srgb: false }, &rgba, 0, 0).err(), Some(ImageError::InvalidInput));

    assert_eq!(images::choose_bc_format(sturdy::assets::TextureKind::NormalMap, false), sturdy::rhi::Format::BC5Unorm);
    assert_eq!(images::choose_bc_format(sturdy::assets::TextureKind::ColorAlpha, true), sturdy::rhi::Format::BC7UnormSrgb);

    let rg = images::pack_metallic_roughness_rg(&rgba, w, h).expect("metallic-roughness RG pack");
    assert_eq!(rg.len(), (w * h * 4) as usize);
    // Output texel = (roughness-in-G, metallic-in-B) -> (R, G), B = 0, A = 255.
    assert_eq!(&rg[..4], &[rgba[1], rgba[2], 0, 255]);
    let orm = images::pack_orm_rgba8(&rgba, &rgba, w, h).expect("ORM pack");
    assert_eq!(orm.len(), (w * h * 4) as usize);
    eprintln!("[images] decode / mip chain / BC compression / packing checks passed");
}

/// Exercises the typed ECS API end to end against the live engine and logs a pass/fail trail via
/// `eprintln!`. This has never run against the real engine before, so every step is checked rather
/// than assumed to work.
fn demonstrate_ecs(engine: &mut Engine<'_>) {
    let mut ecs = engine.ecs();

    // Registration is idempotent: calling it up front is optional (typed spawn/insert helpers
    // register on demand), but doing it explicitly here lets `find` and `query_*` succeed even
    // before any entity carries the component.
    let position_id = ecs.register::<Position>().expect("register Position");
    let velocity_id = ecs.register::<Velocity>().expect("register Velocity");
    eprintln!("ecs: registered Position={position_id:?} Velocity={velocity_id:?}");

    // spawn_one: a single-component entity.
    let a = ecs.spawn_one(Position { x: 1.0, y: 2.0, z: 3.0 }).expect("spawn_one Position");
    eprintln!("ecs: spawned entity a={a:?} with Position only");

    // spawn_builder: a multi-component entity in one call.
    let b = ecs
        .spawn_builder()
        .with(Position { x: 10.0, y: 20.0, z: 30.0 })
        .with(Velocity { x: 0.0, y: -9.8, z: 0.0 })
        .build()
        .expect("spawn_builder Position+Velocity");
    eprintln!("ecs: spawned entity b={b:?} with Position+Velocity");

    // get: read back exactly what was written.
    let a_pos = ecs.get::<Position>(a).expect("get Position on a");
    assert_eq!(a_pos, Position { x: 1.0, y: 2.0, z: 3.0 });
    let b_pos = ecs.get::<Position>(b).expect("get Position on b");
    assert_eq!(b_pos, Position { x: 10.0, y: 20.0, z: 30.0 });
    let b_vel = ecs.get::<Velocity>(b).expect("get Velocity on b");
    assert_eq!(b_vel, Velocity { x: 0.0, y: -9.8, z: 0.0 });
    eprintln!("ecs: read-back matches what was written for a and b");

    // has / insert / remove on entity a, which started with only Position.
    assert!(!ecs.has::<Velocity>(a), "a should not have Velocity yet");
    ecs.insert(a, Velocity { x: 1.0, y: 0.0, z: 0.0 }).expect("insert Velocity on a");
    assert!(ecs.has::<Velocity>(a), "a should have Velocity after insert");
    let a_vel = ecs.get::<Velocity>(a).expect("get Velocity on a after insert");
    assert_eq!(a_vel, Velocity { x: 1.0, y: 0.0, z: 0.0 });
    eprintln!("ecs: insert/has round-trip on a succeeded");

    // set: overwrite in place.
    ecs.set(a, Position { x: 100.0, y: 200.0, z: 300.0 }).expect("set Position on a");
    assert_eq!(ecs.get::<Position>(a).expect("get Position after set"), Position { x: 100.0, y: 200.0, z: 300.0 });
    eprintln!("ecs: set/get round-trip on a succeeded");

    // remove: a goes back to Position-only.
    ecs.remove::<Velocity>(a).expect("remove Velocity from a");
    assert!(!ecs.has::<Velocity>(a), "a should not have Velocity after remove");
    eprintln!("ecs: remove Velocity from a succeeded");

    // query::<(Position,)>: both a and b carry Position.
    let positions = ecs.query::<(Position,)>().expect("query::<(Position,)>");
    assert_eq!(positions.len(), 2, "expected both entities in query::<(Position,)>");
    assert!(positions.iter().any(|(e, (p,))| *e == a && *p == Position { x: 100.0, y: 200.0, z: 300.0 }));
    assert!(positions.iter().any(|(e, (p,))| *e == b && *p == Position { x: 10.0, y: 20.0, z: 30.0 }));
    eprintln!("ecs: query::<(Position,)> returned {} entities as expected", positions.len());

    // query::<(Position, Velocity)>: only b carries both (a's Velocity was removed above).
    let both = ecs.query::<(Position, Velocity)>().expect("query::<(Position, Velocity)>");
    assert_eq!(both.len(), 1, "expected only b in query::<(Position, Velocity)>");
    assert_eq!(both[0].0, b);
    eprintln!("ecs: query::<(Position, Velocity)> returned exactly entity b as expected");

    // Error path: `get` on a component the entity doesn't carry must come back as `EcsError`, not
    // panic.
    match ecs.get::<Velocity>(a) {
        Err(EcsError::MissingComponent) => {
            eprintln!("ecs: get::<Velocity>(a) correctly reported MissingComponent after remove");
        }
        other => panic!("expected MissingComponent, got {other:?}"),
    }

    // Error path: any lookup on a despawned entity must come back as `EcsError::DeadEntity`.
    ecs.despawn(a);
    match ecs.get::<Position>(a) {
        Err(EcsError::DeadEntity) => {
            eprintln!("ecs: get::<Position>(a) correctly reported DeadEntity after despawn");
        }
        other => panic!("expected DeadEntity, got {other:?}"),
    }

    eprintln!("ecs: all typed ECS demonstration checks passed");
}

/// Demo component: hit points, used only to give the generic `World::query` demo below a third
/// component type (beyond `Position`/`Velocity`) and to give `PlayerBundle` something besides a
/// transform.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, bytemuck::Pod, bytemuck::Zeroable)]
struct Health {
    current: f32,
    max: f32,
}

impl sturdy::Component for Health {
    const NAME: &'static str = "hello.demo.health";
}

/// Demo bundle: exercises `#[derive(Bundle)]` / `World::spawn::<PlayerBundle>`. Every field must
/// implement `Component` — the derive walks the struct's fields and registers/spawns each one as
/// its own component, exactly as chaining `SpawnBuilder::with` calls would.
#[derive(Debug, Clone, Copy, Bundle)]
struct PlayerBundle {
    position: Position,
    velocity: Velocity,
    health: Health,
}

/// Demo event: exercises the typed event-channel API (`World::create_event_channel::<T>`,
/// `send_event`, `read_events`).
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, bytemuck::Pod, bytemuck::Zeroable)]
struct DamageEvent {
    amount: f32,
}

impl sturdy::Event for DamageEvent {
    const NAME: &'static str = "hello.demo.damage_event";
}

/// Demo resource: exercises the typed resource API (`World::insert_resource`, `resource`,
/// `set_resource_typed`).
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, bytemuck::Pod, bytemuck::Zeroable)]
struct Score {
    value: u32,
}

impl sturdy::Resource for Score {
    const NAME: &'static str = "hello.demo.score";
}

/// Exercises the four ECS extensions added on top of `demonstrate_ecs` above: the fully generic
/// `World::query`, `#[derive(Bundle)]` + `World::spawn`, typed event channels (including the
/// drain-on-read clear semantics), and typed resources. Logged with an `[ecs-ext]` prefix to keep
/// it easy to pick out from `demonstrate_ecs`'s own `ecs:`-prefixed lines.
/// Twelve distinct one-field components (and resources), for exercising the widest
/// `QueryTuple`/`SystemTuple`/`ResourceTuple` impls.
macro_rules! wide_components {
    ($($name:ident = $label:literal),+ $(,)?) => {
        $(
            #[repr(C)]
            #[derive(Debug, Clone, Copy, PartialEq, bytemuck::Pod, bytemuck::Zeroable)]
            struct $name(u32);
            impl sturdy::Component for $name {
                const NAME: &'static str = $label;
            }
            impl sturdy::Resource for $name {
                const NAME: &'static str = $label;
            }
        )+
    };
}

wide_components!(
    Wide0 = "hello.wide0", Wide1 = "hello.wide1", Wide2 = "hello.wide2", Wide3 = "hello.wide3",
    Wide4 = "hello.wide4", Wide5 = "hello.wide5", Wide6 = "hello.wide6", Wide7 = "hello.wide7",
    Wide8 = "hello.wide8", Wide9 = "hello.wide9", Wide10 = "hello.wide10", Wide11 = "hello.wide11",
);

fn demonstrate_ecs_ext(engine: &mut Engine<'_>) {
    let mut ecs = engine.ecs();

    // -- Task 1: generic World::query over 3+ component types -----------------------------------
    //
    // Three entities: one with all of Position+Velocity+Health, one missing Health, one missing
    // Velocity, so the 3-way query below must return exactly the first.
    let full = ecs
        .spawn_builder()
        .with(Position { x: 1.0, y: 2.0, z: 3.0 })
        .with(Velocity { x: 0.1, y: 0.2, z: 0.3 })
        .with(Health { current: 50.0, max: 100.0 })
        .build()
        .expect("spawn full entity");
    ecs.spawn_builder()
        .with(Position { x: 4.0, y: 5.0, z: 6.0 })
        .with(Velocity { x: 0.0, y: 0.0, z: 0.0 })
        .build()
        .expect("spawn Position+Velocity-only entity");
    ecs.spawn_builder()
        .with(Position { x: 7.0, y: 8.0, z: 9.0 })
        .with(Health { current: 10.0, max: 10.0 })
        .build()
        .expect("spawn Position+Health-only entity");

    let triples = ecs
        .query::<(Position, Velocity, Health)>()
        .expect("generic query over Position+Velocity+Health");
    assert_eq!(triples.len(), 1, "expected exactly the full entity to match the 3-way query");
    assert_eq!(triples[0].0, full);
    assert_eq!(triples[0].1 .2, Health { current: 50.0, max: 100.0 });
    eprintln!(
        "[ecs-ext] World::query::<(Position, Velocity, Health)>() returned exactly the expected entity ({} match)",
        triples.len()
    );

    // -- 12-wide tuples ---------------------------------------------------------------------------
    type Wide = (Wide0, Wide1, Wide2, Wide3, Wide4, Wide5, Wide6, Wide7, Wide8, Wide9, Wide10, Wide11);
    fn assert_system_tuple<Q: sturdy::schedule::SystemTuple>() {}
    fn assert_resource_tuple<Q: sturdy::schedule::ResourceTuple>() {}
    assert_system_tuple::<(R<Wide0>, W<Wide1>, R<Wide2>, R<Wide3>, R<Wide4>, R<Wide5>, R<Wide6>, R<Wide7>, R<Wide8>, R<Wide9>, R<Wide10>, W<Wide11>)>();
    assert_resource_tuple::<(R<Wide0>, W<Wide1>, R<Wide2>, R<Wide3>, R<Wide4>, R<Wide5>, R<Wide6>, R<Wide7>, R<Wide8>, R<Wide9>, R<Wide10>, W<Wide11>)>();
    let wide_entity = ecs
        .spawn_builder()
        .with(Wide0(0)).with(Wide1(1)).with(Wide2(2)).with(Wide3(3)).with(Wide4(4)).with(Wide5(5))
        .with(Wide6(6)).with(Wide7(7)).with(Wide8(8)).with(Wide9(9)).with(Wide10(10)).with(Wide11(11))
        .build()
        .expect("spawn 12-component entity");
    let wide = ecs.query::<Wide>().expect("12-wide query");
    assert_eq!(wide.len(), 1);
    assert_eq!(wide[0].0, wide_entity);
    let row = wide[0].1;
    let values = [row.0 .0, row.1 .0, row.2 .0, row.3 .0, row.4 .0, row.5 .0, row.6 .0, row.7 .0, row.8 .0, row.9 .0, row.10 .0, row.11 .0];
    eprintln!("[ecs-ext] World::query over a 12-tuple -> {values:?}");
    assert_eq!(values, core::array::from_fn(|i| i as u32));

    let player = ecs
        .spawn(PlayerBundle {
            position: Position { x: 0.0, y: 0.0, z: 0.0 },
            velocity: Velocity { x: 1.0, y: 0.0, z: 0.0 },
            health: Health { current: 100.0, max: 100.0 },
        })
        .expect("spawn PlayerBundle via World::spawn");
    let player_health = ecs.get::<Health>(player).expect("get Health on player");
    assert_eq!(player_health, Health { current: 100.0, max: 100.0 });
    eprintln!("[ecs-ext] World::spawn::<PlayerBundle> spawned {player:?}; Health read back matches");

    // -- Task 3: typed event channels, including drain-on-read clear semantics ------------------
    let channel = ecs.create_event_channel::<DamageEvent>().expect("create DamageEvent channel");
    ecs.send_event(channel, DamageEvent { amount: 5.0 }).expect("send DamageEvent 1");
    ecs.send_event(channel, DamageEvent { amount: 12.5 }).expect("send DamageEvent 2");
    ecs.send_event(channel, DamageEvent { amount: 3.25 }).expect("send DamageEvent 3");

    let first_read = ecs.read_events::<DamageEvent>(channel).expect("read DamageEvent batch 1");
    assert_eq!(
        first_read,
        vec![DamageEvent { amount: 5.0 }, DamageEvent { amount: 12.5 }, DamageEvent { amount: 3.25 }],
        "expected all 3 events back in send order"
    );
    eprintln!("[ecs-ext] read_events (1st call): {first_read:?} (count={})", first_read.len());

    // This binding layer never runs the engine's own Ecs::Schedule (see World::read_events_bytes's
    // doc comment), so nothing else clears the channel between calls — read_events itself drains
    // it. A second read right after the first, with no send in between, must therefore come back
    // empty rather than repeating the same events.
    let second_read = ecs.read_events::<DamageEvent>(channel).expect("read DamageEvent batch 2");
    assert!(second_read.is_empty(), "expected an empty batch: read_events drains the channel");
    eprintln!(
        "[ecs-ext] read_events (2nd call, no sends in between): {second_read:?} (count={}) -- confirms drain-on-read",
        second_read.len()
    );

    // -- Task 4: typed resources ------------------------------------------------------------------
    let score_id = ecs.insert_resource(Score { value: 0 }).expect("insert Score resource");
    let score = ecs.resource::<Score>(score_id).expect("read Score resource");
    assert_eq!(score, Score { value: 0 });
    ecs.set_resource_typed(score_id, Score { value: 42 }).expect("update Score resource");
    let updated_score = ecs.resource::<Score>(score_id).expect("read updated Score resource");
    assert_eq!(updated_score, Score { value: 42 });
    eprintln!(
        "[ecs-ext] typed resource Score: inserted={score:?}, after set_resource_typed={updated_score:?}"
    );

    eprintln!("[ecs-ext] all ECS-extension demonstration checks passed");
}

/// Demo resource: counts how many times the schedule demo's global system has ticked. Also gates
/// the one-shot `Commands` calls below to that system's very first dispatch.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, bytemuck::Pod, bytemuck::Zeroable)]
struct ScheduleCounters {
    ticks: u32,
}

impl sturdy::Resource for ScheduleCounters {
    const NAME: &'static str = "hello.demo.schedule_counters";
}

/// Log written by the event/ordering demo's systems, checked from `frame`.
static SCHEDULE_LOG: std::sync::Mutex<Vec<String>> = std::sync::Mutex::new(Vec::new());

/// Event record for the in-system event demo.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, bytemuck::Pod, bytemuck::Zeroable)]
struct DemoPing {
    value: u32,
}

/// Registers global systems that send/read an event channel from inside the scheduler and that are
/// ordered with `Access::before`/`after`; `check_schedule_events_demo` verifies the log.
fn demonstrate_schedule_events(engine: &mut Engine<'_>) {
    use sturdy::schedule::Access;

    let mut ecs = engine.ecs();
    ecs.create_event_channel_bytes("hello.demo.ping", 4).expect("create ping channel");
    let counters = || Access::new().reads_resource::<ScheduleCounters>();

    // Writer: registered first, `before` the token the reader is `after`.
    ecs.add_global_system_events_bytes(
        ScheduleTarget::Update,
        &counters().writes_events("hello.demo.ping").before("hello.demo.ping_order"),
        |_res, events, _commands| {
            events[0].send(DemoPing { value: 42 }).expect("send ping");
            assert!(events[0].send_bytes(&[0; 3]).is_err(), "wrong-size record is rejected");
            SCHEDULE_LOG.lock().unwrap().push("write".into());
        },
    )
    .expect("register event writer system");

    ecs.add_global_system_events_bytes(
        ScheduleTarget::Update,
        &counters().reads_events("hello.demo.ping").after("hello.demo.ping_order"),
        |_res, events, _commands| {
            let pings = events[0].read::<DemoPing>();
            assert!(events[0].send(DemoPing { value: 1 }).is_err(), "read-only channel rejects send");
            SCHEDULE_LOG.lock().unwrap().push(format!("read:{}", pings.iter().map(|p| p.value).sum::<u32>()));
        },
    )
    .expect("register event reader system");
    eprintln!("[schedule-events] registered ordered event writer/reader systems");
}

/// Returns true once the ordered writer->reader pattern has been observed for two ticks.
fn check_schedule_events_demo(frames_waited: u32) -> bool {
    let log = SCHEDULE_LOG.lock().unwrap();
    if log.len() >= 4 {
        for pair in log.chunks_exact(2) {
            assert_eq!(pair[0], "write", "writer runs first each tick: {log:?}");
            assert_eq!(pair[1], "read:42", "reader sees exactly the writer's event each tick: {log:?}");
        }
        eprintln!("[schedule-events] in-system event send/read and before/after ordering verified: {:?}", &log[..4]);
        return true;
    }
    assert!(frames_waited < 600, "event systems never ran; log = {log:?}");
    false
}

/// Registers a real, engine-scheduler-driven system (`sturdy::schedule`) and exercises every
/// `Commands` operation (`spawn_one`, `insert`/`add_component`, `remove_component`, `destroy`)
/// from inside its body, on the system's first tick. Called from `GameLogic::init`; the actual
/// assertions run later from `GameLogic::frame` (see `check_schedule_and_commands_demo`) once the
/// Update schedule has actually had a chance to run and apply the queued commands (`Commands`
/// changes only take effect once `Schedule::run` finishes the whole dispatch, so they cannot be
/// observed synchronously here in `init`).
///
/// Returns the two entities `frame()` needs to check: one `Commands::destroy` should remove, one
/// `Commands::insert`/`remove_component` should mutate.
fn demonstrate_schedule_and_commands(engine: &mut Engine<'_>) -> (Entity, Entity) {
    let mut ecs = engine.ecs();

    let target_for_destroy =
        ecs.spawn_one(Health { current: 1.0, max: 1.0 }).expect("spawn target_for_destroy");
    let target_for_components =
        ecs.spawn_one(Health { current: 2.0, max: 2.0 }).expect("spawn target_for_components");

    // Resolved once up front and captured by value into the system closure: a `Commands` cannot
    // resolve a `ComponentId` itself (see `sturdy::schedule`'s module doc for why), so this is the
    // same pattern any real caller needing `Commands::insert`/`spawn_one` inside a system would use.
    let health_id = ecs.register::<Health>().expect("register Health");
    let velocity_id = ecs.register::<Velocity>().expect("register Velocity");

    ecs.insert_resource(ScheduleCounters { ticks: 0 }).expect("insert ScheduleCounters resource");

    ecs.add_global_system::<(W<ScheduleCounters>,), _>(ScheduleTarget::Update, move |(counters,), commands| {
        if counters.ticks == 0 {
            commands.destroy(target_for_destroy);
            commands.spawn_one(health_id, Health { current: 7.0, max: 7.0 });
            commands.insert(target_for_components, velocity_id, Velocity { x: 9.0, y: 8.0, z: 7.0 });
            commands.remove_component(target_for_components, health_id);
        }
        counters.ticks += 1;
    })
    .expect("register schedule-demo global system");

    eprintln!(
        "[schedule-ext] registered a global system + pre-spawned demo entities; \
         Commands results are checked from frame() once the schedule has actually run"
    );
    (target_for_destroy, target_for_components)
}

/// Polled from `GameLogic::frame` until every `Commands` operation queued by
/// `demonstrate_schedule_and_commands` has visibly applied (or a generous frame budget runs out,
/// in which case it panics rather than silently never checking). Returns `true` once fully
/// verified, so the caller can stop polling.
fn check_schedule_and_commands_demo(
    engine: &mut Engine<'_>,
    target_for_destroy: Entity,
    target_for_components: Entity,
    frames_waited: u32,
) -> bool {
    let mut ecs = engine.ecs();

    let destroyed = !ecs.is_alive(target_for_destroy);
    let spawned = ecs.query::<(Health,)>().expect("query::<(Health,)> in schedule-ext check");
    let spawned_via_commands = spawned.iter().any(|(_, (h,))| *h == Health { current: 7.0, max: 7.0 });
    let has_velocity = ecs.has::<Velocity>(target_for_components);
    let has_health = ecs.has::<Health>(target_for_components);

    if !(destroyed && spawned_via_commands && has_velocity && !has_health) {
        assert!(
            frames_waited < 300,
            "[schedule-ext] Commands never applied after {frames_waited} frames: \
             destroyed={destroyed} spawned_via_commands={spawned_via_commands} \
             has_velocity={has_velocity} has_health={has_health}"
        );
        return false;
    }

    eprintln!(
        "[schedule-ext] Commands verified after {frames_waited} frame(s): target_for_destroy alive=false, \
         Commands::spawn_one created a Health{{current:7,max:7}} entity, target_for_components gained \
         Velocity (Commands::insert) and lost Health (Commands::remove_component)"
    );
    eprintln!("[schedule-ext] all Commands demonstration checks passed");
    true
}

/// Exercises the reflection-extension surface added on top of the original read-only type/field/
/// enum descriptors (`sturdy::reflection`): live instance field read/write, default construction,
/// zero-argument method invocation, container/optional element introspection, attributes, and
/// event descriptors. Logged with a `[reflection-ext]` prefix.
///
/// No engine-defined gameplay type is reflected by default (the vendored engine reflects only its
/// own internals), and the `Position`/`Velocity`/... structs above are plain Rust ECS components,
/// never registered with `SFT::Reflection::TypeRegistry` -- so this exercises
/// `reflection::register_demo_type()`'s tiny C++ demo type (`"sturdy_rs.demo.actor"`, declared in
/// `sturdy-sys/cpp/sturdy_rs/reflection.cpp`) instead. It has trivial float/int fields, a
/// `std::vector<int>` field, a `std::optional<int>` field, a zero-arg method, a non-zero-arg method
/// (descriptor only), an attributed field, and a declared event -- enough to cover every piece of
/// the new surface end to end.
fn demonstrate_reflection() {
    const TYPE_NAME: &str = "sturdy_rs.demo.actor";

    reflection::register_demo_type();

    let info = reflection::find_type(TYPE_NAME).expect("sturdy_rs.demo.actor must be registered");
    eprintln!(
        "[reflection-ext] find_type({TYPE_NAME:?}) -> size={} align={} field_count={} method_count={} has_default_constructor={}",
        info.size, info.align, info.field_count, info.method_count, info.has_default_constructor
    );
    assert!(info.has_default_constructor, "DemoActor must be default-constructible");

    let fields = reflection::fields(TYPE_NAME);
    let field_names: Vec<&str> = fields.iter().map(|f| f.name.as_str()).collect();
    eprintln!("[reflection-ext] fields: {field_names:?}");
    assert!(field_names.contains(&"x"));
    assert!(field_names.contains(&"health"));

    // -- Constructors: default-construct a real instance -----------------------------------------
    let mut instance = reflection::construct_default(TYPE_NAME).expect("default construction must succeed");
    eprintln!("[reflection-ext] default-constructed a {} byte instance", instance.bytes().len());

    let constructor_list = reflection::constructors(TYPE_NAME);
    eprintln!(
        "[reflection-ext] parameterized constructors: {} (expected 0 -- DemoActor declares none via SFT_REFLECT_CONSTRUCTOR)",
        constructor_list.len()
    );
    assert!(constructor_list.is_empty());

    // -- Live field read/write, cross-checked through a *different* code path (method invocation) -
    //
    // Reading the field back through reflection after writing it only proves the write and the
    // read agree with each other -- not that it actually landed in the instance's real memory.
    // Invoking `double_health` (a compiled C++ method reading `this->health` directly) after
    // writing `health` through reflection is an independent confirmation: if the write hadn't
    // really landed, the compiled method would still see the old value.
    let initial_x: f32 = reflection::read_field_as(&fields, "x", instance.bytes()).expect("read x");
    let initial_health: i32 = reflection::read_field_as(&fields, "health", instance.bytes()).expect("read health");
    eprintln!("[reflection-ext] initial x={initial_x}, health={initial_health} (expected 0, 100 -- the C++ default member initializers)");
    assert_eq!(initial_x, 0.0);
    assert_eq!(initial_health, 100);

    reflection::write_field_value::<f32>(&fields, "x", instance.bytes_mut(), 42.5).expect("write x");
    let read_back_x: f32 = reflection::read_field_as(&fields, "x", instance.bytes()).expect("read x after write");
    eprintln!("[reflection-ext] wrote x=42.5, read back x={read_back_x}");
    assert_eq!(read_back_x, 42.5);

    reflection::write_field_value::<i32>(&fields, "health", instance.bytes_mut(), 55).expect("write health");

    // -- Methods: enumerate, then invoke the zero-arg one and confirm it saw the write above ------
    let method_list = reflection::methods(TYPE_NAME);
    eprintln!(
        "[reflection-ext] methods: {:?}",
        method_list.iter().map(|m| (m.name.as_str(), m.param_type_names.len(), m.return_type_name.as_str())).collect::<Vec<_>>()
    );
    assert!(method_list.iter().any(|m| m.name == "double_health" && m.param_type_names.is_empty()));
    assert!(method_list.iter().any(|m| m.name == "heal" && m.param_type_names.len() == 1));

    let doubled = reflection::invoke_method0(TYPE_NAME, "double_health", instance.bytes_mut()).expect("invoke double_health");
    eprintln!("[reflection-ext] invoke_method0(double_health) -> {doubled:?} (expected SignedInt(110) = 2 * the health=55 written above)");
    assert_eq!(doubled, reflection::MethodValue::SignedInt(110));

    // Read health back through reflection too, confirming both paths (the compiled method call
    // above, and a direct offset re-read here) agree with what was written.
    let health_after: i32 = reflection::read_field_as(&fields, "health", instance.bytes()).expect("read health after write");
    assert_eq!(health_after, 55);
    eprintln!("[reflection-ext] health re-read via reflection after the write = {health_after} -- matches what the method call saw");

    // -- Container/optional element introspection --------------------------------------------------
    let tags_shape = reflection::field_container(TYPE_NAME, "tags").expect("tags must be a container");
    eprintln!("[reflection-ext] field_container(tags) -> {tags_shape:?}");
    assert_eq!(tags_shape.kind, reflection::ContainerKind::Vector);
    assert_eq!(tags_shape.element_type_name.as_deref(), Some("i32"));

    let target_shape = reflection::field_container(TYPE_NAME, "target").expect("target must be an optional");
    eprintln!("[reflection-ext] field_container(target) -> {target_shape:?}");
    assert_eq!(target_shape.kind, reflection::ContainerKind::Optional);
    assert_eq!(target_shape.element_type_name.as_deref(), Some("i32"));

    assert!(reflection::field_container(TYPE_NAME, "x").is_none(), "a trivial field has no container shape");

    // -- Attributes -----------------------------------------------------------------------------
    let health_attributes = reflection::field_attributes(TYPE_NAME, "health");
    eprintln!("[reflection-ext] health field attributes: {health_attributes:?}");
    assert!(health_attributes.iter().any(|a| a.name == "units" && a.value == reflection::AttributeValue::String("hp".to_owned())));
    assert!(health_attributes.iter().any(|a| a.name == "min" && a.value == reflection::AttributeValue::SignedInt(0)));

    // -- Events: enumerate, look up by name, and fire (no subscriber, so a defined no-op) ---------
    let event_list = reflection::events(TYPE_NAME);
    eprintln!("[reflection-ext] events: {:?}", event_list.iter().map(|e| e.name.as_str()).collect::<Vec<_>>());
    assert!(event_list.iter().any(|e| e.name == "on_died"));

    let on_died = reflection::find_event(TYPE_NAME, "on_died").expect("find_event(on_died)");
    eprintln!("[reflection-ext] find_event(on_died) -> {on_died:?}");
    assert!(on_died.param_type_names.is_empty(), "on_died is declared with no parameters");
    assert!(reflection::find_event(TYPE_NAME, "no_such_event").is_none());

    let fired = reflection::fire_event(TYPE_NAME, "on_died", instance.bytes_mut(), &[]);
    eprintln!("[reflection-ext] fire_event(on_died) -> {fired} (expected true: arity matches, even with no subscriber)");
    assert!(fired);
    assert!(
        !reflection::fire_event(TYPE_NAME, "on_died", instance.bytes_mut(), &[&[0u8; 4]]),
        "firing with the wrong argument count must fail closed rather than fire partially"
    );

    // -- Static fields: independent of any instance's bytes -----------------------------------------
    let initial_spawn_count: i32 =
        reflection::get_static_field_as(&fields, TYPE_NAME, "spawn_count").expect("read spawn_count");
    eprintln!("[reflection-ext] static field spawn_count = {initial_spawn_count} (expected 0, the C++ static initializer)");
    assert_eq!(initial_spawn_count, 0);

    reflection::set_static_field_value::<i32>(&fields, TYPE_NAME, "spawn_count", 7).expect("write spawn_count");
    let updated_spawn_count: i32 =
        reflection::get_static_field_as(&fields, TYPE_NAME, "spawn_count").expect("read spawn_count after write");
    eprintln!("[reflection-ext] static field spawn_count after write = {updated_spawn_count}");
    assert_eq!(updated_spawn_count, 7);

    // A second, independently default-constructed instance must see the *same* static storage --
    // proof this isn't accidentally reading/writing per-instance bytes.
    let other_instance = reflection::construct_default(TYPE_NAME).expect("construct second DemoActor");
    let shared_spawn_count: i32 = reflection::get_static_field_as(&fields, TYPE_NAME, "spawn_count")
        .expect("read spawn_count via a fresh lookup");
    eprintln!(
        "[reflection-ext] spawn_count read again after constructing a second, unrelated instance = {shared_spawn_count} (expected still 7 -- static storage is shared, not per-instance)"
    );
    assert_eq!(shared_spawn_count, 7);
    drop(other_instance);

    // -- Container element access: resize/set/get/len on the `tags` field (Vec<i32>) --------------
    assert_eq!(reflection::container_len(TYPE_NAME, "tags", instance.bytes()), 0, "tags starts empty");
    assert!(
        reflection::container_resize(TYPE_NAME, "tags", instance.bytes_mut(), 3),
        "resize tags to 3 elements"
    );
    assert_eq!(reflection::container_len(TYPE_NAME, "tags", instance.bytes()), 3);
    for (index, value) in [10_i32, 20, 30].into_iter().enumerate() {
        assert!(
            reflection::container_set_element_value(TYPE_NAME, "tags", instance.bytes_mut(), index, value),
            "set tags[{index}]"
        );
    }
    let tags_read_back: Vec<i32> = (0..3)
        .map(|index| {
            reflection::container_get_element_as::<i32>(TYPE_NAME, "tags", instance.bytes(), index)
                .unwrap_or_else(|| panic!("get tags[{index}]"))
        })
        .collect();
    eprintln!("[reflection-ext] tags after resize(3) + set_element(0..3, [10,20,30]) -> {tags_read_back:?}");
    assert_eq!(tags_read_back, vec![10, 20, 30]);
    assert!(
        reflection::container_get_element_as::<i32>(TYPE_NAME, "tags", instance.bytes(), 3).is_none(),
        "index 3 is out of range for a 3-element container"
    );

    // -- Overrides, hooks, and event subscription ----------------------------------------------
    {
        use std::sync::atomic::{AtomicU32, Ordering};
        let hook_calls = Arc::new(AtomicU32::new(0));
        let health = fields.iter().find(|f| f.name == "health").expect("health field").clone();

        let before = {
            let hook_calls = hook_calls.clone();
            reflection::add_method_hook(TYPE_NAME, "double_health", reflection::HookTiming::Before, move |_| {
                hook_calls.fetch_add(1, Ordering::SeqCst);
            })
            .expect("add before hook")
        };
        let after = {
            let hook_calls = hook_calls.clone();
            reflection::add_method_hook(TYPE_NAME, "double_health", reflection::HookTiming::After, move |_| {
                hook_calls.fetch_add(1, Ordering::SeqCst);
            })
            .expect("add after hook")
        };
        // The override reads `health` straight out of the receiver bytes and returns 3x instead of
        // 2x -- proving both the receiver slice and the return slot are wired to the real call.
        let overridden = reflection::override_method(TYPE_NAME, "double_health", move |call| {
            let object = call.object.as_deref().expect("instance method has a receiver");
            let value = i32::from_ne_bytes(object[health.offset..health.offset + 4].try_into().unwrap());
            call.ret.as_deref_mut().expect("i32 return slot").copy_from_slice(&(value * 3).to_ne_bytes());
        })
        .expect("override double_health");

        let with_override = reflection::invoke_method0(TYPE_NAME, "double_health", instance.bytes_mut()).expect("invoke overridden");
        let calls_after_one = hook_calls.load(Ordering::SeqCst);
        eprintln!(
            "[reflection-ext] override_method(double_health -> 3x) invoke = {with_override:?} (expected SignedInt(165) = 3 * 55); before+after hook calls = {calls_after_one} (expected 2)"
        );
        assert_eq!(with_override, reflection::MethodValue::SignedInt(165));
        assert_eq!(calls_after_one, 2);

        drop(overridden);
        let restored = reflection::invoke_method0(TYPE_NAME, "double_health", instance.bytes_mut()).expect("invoke restored");
        eprintln!("[reflection-ext] after dropping the override: {restored:?} (expected SignedInt(110), the original body)");
        assert_eq!(restored, reflection::MethodValue::SignedInt(110));

        drop(before);
        drop(after);
        let calls_before_last = hook_calls.load(Ordering::SeqCst);
        let _ = reflection::invoke_method0(TYPE_NAME, "double_health", instance.bytes_mut()).expect("invoke unhooked");
        let calls_final = hook_calls.load(Ordering::SeqCst);
        eprintln!("[reflection-ext] hook calls after dropping both hooks: {calls_before_last} -> {calls_final} (expected unchanged)");
        assert_eq!(calls_before_last, 4, "the restored call must still have run both hooks");
        assert_eq!(calls_final, calls_before_last, "dropped hooks must not run");

        let fired = Arc::new(AtomicU32::new(0));
        let subscription = {
            let fired = fired.clone();
            reflection::subscribe_event(TYPE_NAME, "on_died", move |call| {
                assert!(call.object.is_some() && call.args.is_empty());
                fired.fetch_add(1, Ordering::SeqCst);
            })
            .expect("subscribe on_died")
        };
        assert!(reflection::fire_event(TYPE_NAME, "on_died", instance.bytes_mut(), &[]));
        let fired_once = fired.load(Ordering::SeqCst);
        drop(subscription);
        assert!(reflection::fire_event(TYPE_NAME, "on_died", instance.bytes_mut(), &[]));
        let fired_final = fired.load(Ordering::SeqCst);
        eprintln!("[reflection-ext] subscribe_event(on_died): fires seen = {fired_once}, after unsubscribing = {fired_final} (expected 1, 1)");
        assert_eq!((fired_once, fired_final), (1, 1));

        assert!(matches!(
            reflection::override_method(TYPE_NAME, "no_such_method", |_| {}),
            Err(reflection::HookError::NotFound)
        ));
    }

    // -- Runtime overlay: presentation-only name/attribute overrides ----------------------------
    {
        use reflection::OverlayTarget;
        let health = OverlayTarget::Field("health");
        let static_attrs = reflection::effective_attributes(TYPE_NAME, health).len();
        assert!(reflection::set_name_override(TYPE_NAME, OverlayTarget::Type, "Actor"));
        assert!(reflection::set_name_override(TYPE_NAME, health, "Hit Points"));
        assert!(reflection::set_name_override(TYPE_NAME, OverlayTarget::Method("double_health"), "Double HP"));
        let tooltip = reflection::Attribute {
            name: "tooltip".into(),
            value: reflection::AttributeValue::String("Remaining health".into()),
        };
        assert!(reflection::add_attribute_override(TYPE_NAME, health, &tooltip));
        let names = (
            reflection::effective_name(TYPE_NAME, OverlayTarget::Type),
            reflection::effective_name(TYPE_NAME, health),
            reflection::effective_name(TYPE_NAME, OverlayTarget::Method("double_health")),
        );
        let attrs = reflection::effective_attributes(TYPE_NAME, health);
        let declared_still = reflection::fields(TYPE_NAME).iter().any(|f| f.name == "health");
        eprintln!(
            "[reflection-ext] overlay names = {names:?}; health attrs {static_attrs} static -> {} effective (last = {:?}); declared name untouched = {declared_still}",
            attrs.len(),
            attrs.last()
        );
        assert_eq!(names, (Some("Actor".into()), Some("Hit Points".into()), Some("Double HP".into())));
        assert_eq!(attrs.len(), static_attrs + 1);
        assert_eq!(attrs.last(), Some(&tooltip));
        assert!(declared_still && reflection::find_type(TYPE_NAME).is_some());

        assert!(reflection::clear_name_override(TYPE_NAME, OverlayTarget::Type));
        assert!(reflection::clear_overlay(TYPE_NAME, health));
        assert!(reflection::clear_overlay(TYPE_NAME, OverlayTarget::Method("double_health")));
        assert_eq!(reflection::effective_name(TYPE_NAME, OverlayTarget::Type).as_deref(), Some(TYPE_NAME));
        assert_eq!(reflection::effective_name(TYPE_NAME, health).as_deref(), Some("health"));
        assert_eq!(reflection::effective_attributes(TYPE_NAME, health).len(), static_attrs);
        assert!(!reflection::set_name_override(TYPE_NAME, OverlayTarget::Field("nope"), "x"));
        assert!(reflection::effective_name(TYPE_NAME, OverlayTarget::Method("nope")).is_none());
        eprintln!("[reflection-ext] overlay cleared: names and attributes back to their declared values");
    }

    // `instance` destroys itself (running DemoActor's real C++ destructor, which frees `tags`'
    // heap storage) when dropped here.
    drop(instance);

    eprintln!("[reflection-ext] all reflection-extension demonstration checks passed");
}

/// Registers a brand-new reflected type from Rust (`reflection::TypeBuilder`) and drives it
/// through the same generic reflection surface a C++-declared type gets: descriptors, attributes,
/// default construction, Rust-bodied methods (zero-arg and byte-marshalled), hooks, events,
/// nesting, and unregistration. Logged with a `[reflection-rt]` prefix.
/// Exercises `sturdy::hot_reload`: loads the `hot_module` cdylib (built by `cargo build
/// --workspace`; skipped with a note if it isn't beside this executable), reloads it, and watches a
/// file for modification.
fn demonstrate_hot_reload() {
    use sturdy::hot_reload::{FileWatcher, HotModule, HotReloadError};

    let dir = std::env::current_exe().expect("exe path").parent().expect("exe dir").to_path_buf();
    let library = dir.join(format!("{}hot_module{}", std::env::consts::DLL_PREFIX, std::env::consts::DLL_SUFFIX));

    let mut module = HotModule::new();
    assert!(!module.is_loaded() && module.version().is_none());
    assert!(matches!(module.load(dir.join("does_not_exist.so")), Err(HotReloadError::LibraryLoadFailed(_))));
    assert!(!module.is_loaded(), "a failed load leaves the module unloaded");

    if library.exists() {
        module.load(&library).expect("load hot_module");
        assert_eq!(module.version(), Some(7));
        module.reload(&library).expect("reload hot_module");
        assert_eq!(module.version(), Some(7));
        module.unload();
        assert!(!module.is_loaded());
        eprintln!("[hot-reload] load / reload / unload checks passed");
    } else {
        eprintln!("[hot-reload] {} not found; build the workspace to exercise load/reload", library.display());
    }

    let watched = std::env::temp_dir().join(format!("sturdy_hot_reload_watch_{}", std::process::id()));
    let mut watcher = FileWatcher::new(&watched);
    assert!(!watcher.poll(), "a missing file reports no change");
    std::fs::write(&watched, b"a").expect("write watched file");
    assert!(!watcher.poll(), "first sight of the file only records its time");
    std::thread::sleep(std::time::Duration::from_millis(50));
    std::fs::write(&watched, b"b").expect("rewrite watched file");
    assert!(watcher.poll(), "modification is reported");
    assert!(!watcher.poll(), "and only once");
    std::fs::remove_file(&watched).ok();
    eprintln!("[hot-reload] file watcher checks passed");
}

/// Exercises the full camera model and `CameraRig` math against the live engine.
#[allow(deprecated)] // glam 0.33 deprecates `Mat4::orthographic_rh`; fine for a demo.
fn demonstrate_camera_rig() {
    use sturdy::camera::{Containment, Projection};
    use sturdy::{Camera, glam};
    use glam::{vec2, vec3, Vec3};

    let close = |a: f32, b: f32, eps: f32, what: &str| assert!((a - b).abs() < eps, "{what}: {a} vs {b}");
    let near_vec = |a: Vec3, b: Vec3, what: &str| assert!((a - b).length() < 1e-3, "{what}: {a:?} vs {b:?}");

    let mut rig = Camera::perspective().at(vec3(0.0, 0.0, 5.0)).fov_degrees(60.0).rig(16.0 / 9.0);
    rig.look_at(Vec3::ZERO, Vec3::Y);
    near_vec(rig.forward(), vec3(0.0, 0.0, -1.0), "look_at forward");
    close(rig.distance_to(Vec3::ZERO), 5.0, 1e-4, "distance_to");

    // World <-> screen round trip, and the picking ray through the centre of the screen.
    let viewport = vec2(1600.0, 900.0);
    let center = rig.project(Vec3::ZERO, viewport).expect("origin projects");
    close(center.x, 800.0, 0.5, "projected x");
    close(center.y, 450.0, 0.5, "projected y");
    near_vec(rig.unproject(center, viewport).expect("unproject"), Vec3::ZERO, "unproject round trip");
    let ray = rig.screen_ray(vec2(800.0, 450.0), viewport).expect("center ray");
    near_vec(ray.direction.normalize(), rig.forward(), "center ray direction");
    // A point at or behind the eye plane has no projection (dividing through w would mirror it onto the screen).
    assert!(rig.project(vec3(0.0, 0.0, 50.0), viewport).is_none(), "behind-camera points do not project");
    assert!((0.0..=1.0).contains(&center.z), "in-front depth is inside 0..=1");

    // Frustum queries.
    assert!(rig.sees_point(Vec3::ZERO) && !rig.sees_point(vec3(0.0, 0.0, 50.0)));
    assert!(rig.sees_sphere(vec3(0.0, 0.0, 0.0), 1.0) && !rig.sees_sphere(vec3(500.0, 0.0, 0.0), 1.0));
    assert_eq!(rig.sees_bounds(vec3(-1.0, -1.0, -1.0), vec3(1.0, 1.0, 1.0)), Containment::Inside);
    assert_eq!(rig.sees_bounds(vec3(400.0, 400.0, 400.0), vec3(401.0, 401.0, 401.0)), Containment::Outside);

    // Movement helpers.
    rig.orbit(Vec3::ZERO, 90.0, 0.0);
    close(rig.distance_to(Vec3::ZERO), 5.0, 1e-3, "orbit keeps its distance to the pivot");
    let before = rig.camera().position;
    rig.dolly(1.0);
    close((rig.camera().position - before).length(), 1.0, 1e-3, "dolly moves along the view direction");
    rig.frame_sphere(Vec3::ZERO, 2.0, 1.1);
    assert!(rig.sees_sphere(Vec3::ZERO, 2.0), "frame_sphere leaves the sphere in view");

    // Physical lens: a 50mm lens on a 24mm-tall sensor has a 2*atan(12/50) vertical FOV.
    let lens = Camera::perspective().focal_length_mm(50.0).rig(1.5).camera();
    let Projection::Perspective { vertical_fov_degrees } = lens.projection else { panic!("perspective") };
    close(vertical_fov_degrees, 2.0 * (12.0f32 / 50.0).atan().to_degrees(), 0.05, "focal length drives the FOV");
    close(lens.focal_length_mm.unwrap(), 50.0, 1e-3, "focal length reads back");

    // Exposure: f/2.8, 1/60 s, ISO 100 => EV100 = log2(2.8^2 * 60) ~ 8.88.
    let exposed = Camera::perspective().exposure(2.8, 1.0 / 60.0, 100.0).rig(1.0);
    close(exposed.ev100(), (2.8f32 * 2.8 * 60.0).log2(), 0.01, "ev100");
    let brighter = Camera::perspective().exposure(2.8, 1.0 / 60.0, 100.0).exposure_compensation_ev(1.0).rig(1.0);
    assert!(brighter.exposure_multiplier() != exposed.exposure_multiplier(), "exposure compensation changes the multiplier");

    // Full state survives a round trip through the engine object.
    let desc = Camera::perspective()
        .at(vec3(1.0, 2.0, 3.0))
        .orientation(glam::Quat::from_rotation_y(0.5))
        .clip(0.1, 250.0)
        .exposure(4.0, 1.0 / 125.0, 400.0)
        .focus(7.5, 9)
        .priority(3)
        .culling_mask(0b1010)
        .clear_color(glam::vec4(0.1, 0.2, 0.3, 1.0))
        .render_scale(0.75)
        .viewport(glam::vec4(0.25, 0.0, 0.5, 1.0))
        .reverse_z(true);
    let back = desc.rig(2.0).camera();
    near_vec(back.position, desc.position, "position");
    close(back.far_clip, 250.0, 1e-3, "far clip");
    close(back.iso, 400.0, 1e-3, "iso");
    assert_eq!((back.aperture_blades, back.priority, back.culling_mask), (9, 3, 0b1010));
    assert!(back.reverse_z && back.active);
    close(back.render_scale, 0.75, 1e-6, "render scale");
    assert_eq!(back.normalized_viewport, glam::vec4(0.25, 0.0, 0.5, 1.0));
    assert!(back.orientation.unwrap().dot(desc.orientation.unwrap()).abs() > 0.9999, "orientation");

    // Custom projection.
    let custom = glam::Mat4::orthographic_rh(-2.0, 2.0, -2.0, 2.0, 0.1, 10.0);
    let custom_rig = Camera::perspective().custom_projection(custom).rig(1.0);
    assert!(matches!(custom_rig.camera().projection, Projection::Custom(_)));

    // Temporal history: absent until committed, then the previous view-projection follows.
    let mut rig = Camera::perspective().at(vec3(0.0, 0.0, 5.0)).rig(1.0);
    assert!(!rig.has_history());
    rig.commit_frame();
    assert!(rig.has_history());
    let committed = rig.view_projection_matrix();
    rig.translate_world(vec3(1.0, 0.0, 0.0));
    assert!(rig.previous_view_projection_matrix().abs_diff_eq(committed, 1e-5), "previous VP is the committed one");
    assert!(!rig.view_projection_matrix().abs_diff_eq(committed, 1e-5), "current VP moved");
    rig.reset_history();
    assert!(!rig.has_history());
    eprintln!("[camera] full camera model / rig math / history checks passed");
}

/// Exercises `sturdy::geometry` (splines, frustum culling) plus topology/main-thread helpers.
#[allow(deprecated)] // glam 0.33 deprecates `Mat4::perspective_rh`/`look_at_rh`; fine for a demo.
fn demonstrate_geometry_and_threads() {
    use sturdy::geometry::{Frustum, Spline3};
    use glam::{vec3, Mat4, Vec3};

    let spline = Spline3::new(&[vec3(0.0, 0.0, 0.0), vec3(10.0, 0.0, 0.0), vec3(20.0, 0.0, 0.0)], 32);
    assert!(spline.valid() && spline.segment_count() == 2);
    assert!((spline.position(0.0) - vec3(0.0, 0.0, 0.0)).length() < 1e-4);
    assert!((spline.position(2.0) - vec3(20.0, 0.0, 0.0)).length() < 1e-4);
    assert!((spline.total_length() - 20.0).abs() < 0.1, "collinear waypoints: length {}", spline.total_length());
    let halfway = spline.position(spline.t_at_arc_length(10.0));
    assert!((halfway.x - 10.0).abs() < 0.2, "arc-length parameterization: {halfway:?}");
    assert!(spline.tangent(1.0).x > 0.0);
    assert!(!Spline3::new(&[vec3(1.0, 1.0, 1.0)], 32).valid(), "one waypoint is not a spline");

    let view_projection = Mat4::perspective_rh(1.0, 1.0, 0.1, 100.0)
        * Mat4::look_at_rh(vec3(0.0, 0.0, 5.0), Vec3::ZERO, Vec3::Y);
    let frustum = Frustum::from_view_projection(view_projection);
    assert!(frustum.intersects_sphere(Vec3::ZERO, 1.0), "sphere at the look target is visible");
    assert!(!frustum.intersects_sphere(vec3(0.0, 0.0, 500.0), 1.0), "sphere behind the camera is culled");
    assert!(!frustum.intersects_sphere(vec3(1000.0, 0.0, 0.0), 1.0), "sphere far off to the side is culled");

    let logical = sturdy::diagnostics::ranked_logical_cores();
    assert!(!logical.is_empty(), "topology reports at least one logical core");
    assert!(!sturdy::diagnostics::ranked_physical_cores().is_empty());
    sturdy::diagnostics::run_on_main_thread(|| MAIN_THREAD_RAN.store(true, std::sync::atomic::Ordering::SeqCst));
    eprintln!("[geometry] spline / frustum / topology checks passed; main-thread task queued");
}

static MAIN_THREAD_RAN: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

/// Exercises moving dock panels between workspaces (the cross-window docking primitive).
fn demonstrate_dock_transfer() {
    use sturdy::ui::DockWorkspace;
    let mut origin = DockWorkspace::new("xfer-origin");
    let mut target = DockWorkspace::new("xfer-target");
    assert!(origin.add_panel("inspector", "Inspector", true, None));
    assert!(origin.add_panel("console", "Console", false, None));
    assert!(target.add_panel("scene", "Scene", true, None));
    assert_eq!(origin.panel_info("console"), Some(("Console".to_string(), false)));
    assert_eq!(origin.panel_info("nope"), None);
    assert!(origin.active_tab_drag().is_none());

    assert!(origin.transfer_panel(&mut target, "inspector", None));
    assert!(!origin.has_panel("inspector") && target.has_panel("inspector"));
    assert_eq!(target.panel_info("inspector"), Some(("Inspector".to_string(), true)), "metadata travels with the panel");
    assert!(!origin.transfer_panel(&mut target, "inspector", None), "already moved");
    assert!(origin.add_panel("scene", "Scene again", true, None));
    assert!(!origin.transfer_panel(&mut target, "scene", None), "target rejects a duplicate id");
    assert!(origin.has_panel("scene"), "a rejected transfer leaves the panel where it was");
    assert!(origin.transfer_panel(&mut target, "console", None));
    assert!(!origin.has_panel("console"));
    eprintln!("[dock-transfer] cross-workspace panel transfer checks passed");
}

/// Exercises synthetic UI input: inject pointer/text state and read it back through the snapshots.
fn demonstrate_ui_input_injection(engine: &mut Engine<'_>) {
    let mut ui = engine.ui();
    ui.set_pointer_position(glam::vec2(12.0, 34.0));
    ui.set_pointer_down(true);
    ui.add_pointer_scroll_delta(glam::vec2(0.0, 3.0));
    ui.set_pointer_consumed(true);
    let pointer = ui.pointer_state();
    assert_eq!(pointer.position, glam::vec2(12.0, 34.0));
    assert!(pointer.down && pointer.pressed_this_frame && pointer.consumed);
    assert_eq!(pointer.scroll_delta, glam::vec2(0.0, 3.0));
    ui.clear_pointer_transitions();
    ui.clear_pointer_scroll_delta();
    ui.set_pointer_down(false);
    ui.set_pointer_consumed(false);
    ui.clear_pointer_transitions();
    let pointer = ui.pointer_state();
    assert!(!pointer.down && !pointer.pressed_this_frame && pointer.scroll_delta == glam::Vec2::ZERO);

    ui.type_text("héllo");
    assert_eq!(ui.text_input().typed_text, "héllo");
    ui.clear_text_input_transitions();
    assert_eq!(ui.text_input().typed_text, "");
    ui.set_composition("にほん", 3, 0);
    let input = ui.text_input();
    assert!(input.composing && input.composition_text == "にほん");
    ui.set_composition("", 0, 0);
    ui.send_edit_key(Key::Backspace, true, false);
    ui.clear_text_input_transitions();
    eprintln!("[ui-input] synthetic pointer/text input checks passed");
}

/// Exercises by-name reflected field access on ECS components (`World::get_field`/`set_field`).
fn demonstrate_ecs_reflection(engine: &mut Engine<'_>) {
    #[repr(C)]
    #[derive(Clone, Copy, Debug, PartialEq, bytemuck::Pod, bytemuck::Zeroable)]
    struct Crate {
        hp: i32,
        weight: f32,
    }
    impl sturdy::Component for Crate {
        const NAME: &'static str = "hello.ecsrefl.crate";
    }
    const TYPE: &str = "hello.ecsrefl.crate";

    reflection::TypeBuilder::with_default(TYPE, &Crate { hp: 1, weight: 1.0 })
        .field("hp", 0, "i32")
        .field("weight", 4, "f32")
        .register()
        .expect("register reflected crate type");

    let mut world = engine.ecs();
    let entity = world.spawn_one(Crate { hp: 40, weight: 2.5 }).expect("spawn crate");
    assert_eq!(world.get_field::<Crate, i32>(entity, TYPE, "hp").expect("read hp"), 40);
    assert_eq!(world.get_field::<Crate, f32>(entity, TYPE, "weight").expect("read weight"), 2.5);
    world.set_field::<Crate, i32>(entity, TYPE, "hp", 7).expect("write hp");
    assert_eq!(
        world.get::<Crate>(entity).unwrap(),
        Crate { hp: 7, weight: 2.5 },
        "writing one field leaves the others untouched"
    );
    assert!(world.get_field::<Crate, i32>(entity, TYPE, "nope").is_err(), "unknown field is an error");
    assert!(world.get_field::<Crate, i32>(entity, "hello.no.such.type", "hp").is_err(), "unknown type is an error");
    assert!(world.get_field::<Crate, f64>(entity, TYPE, "hp").is_err(), "field size mismatch is an error");
    world.despawn(entity);

    // Zero-sized tag (marker) components.
    assert!(world.register_component("hello.demo.enemy_tag", 0, 1).is_err(), "raw zero-size registration is rejected");
    let tag = world.register_tag("hello.demo.enemy_tag").expect("register tag");
    assert_eq!(world.register_tag("hello.demo.enemy_tag").unwrap(), tag, "registering a tag twice is idempotent");
    let tagged = world.spawn_one(Crate { hp: 3, weight: 1.0 }).expect("spawn taggable entity");
    let plain = world.spawn_one(Crate { hp: 4, weight: 1.0 }).expect("spawn plain entity");
    assert!(!world.has_tag(tagged, tag));
    world.add_tag(tagged, tag).expect("add tag");
    assert!(world.has_tag(tagged, tag) && !world.has_tag(plain, tag));
    let crate_id = world.find::<Crate>().expect("Crate registered");
    let snapshot = world.query_bytes(&[crate_id, tag]).expect("query with a tag");
    assert_eq!(snapshot.entities.len(), 1, "the tag narrows the query to tagged entities");
    world.remove_tag(tagged, tag).expect("remove tag");
    assert!(!world.has_tag(tagged, tag));
    world.despawn(tagged);
    world.despawn(plain);
    eprintln!("[ecs-reflection] by-name field read/write checks passed");
}

fn demonstrate_reflection_runtime_types() {
    use std::sync::atomic::{AtomicI32, AtomicU32, Ordering};

    #[repr(C)]
    #[derive(Clone, Copy, Debug, PartialEq, bytemuck::Pod, bytemuck::Zeroable)]
    struct RuntimeCrate {
        hp: i32,
        weight: f32,
    }

    const CRATE: &str = "hello.runtime.crate";
    const PAIR: &str = "hello.runtime.crate_pair";

    fn hp_of(call: &reflection::ReflectedCall<'_>) -> i32 {
        i32::from_ne_bytes(call.object.as_deref().expect("receiver")[0..4].try_into().unwrap())
    }

    reflection::TypeBuilder::with_default(CRATE, &RuntimeCrate { hp: 40, weight: 2.5 })
        .field_with_attributes(
            "hp",
            0,
            "i32",
            &[reflection::Attribute { name: "units".into(), value: reflection::AttributeValue::String("hp".into()) }],
        )
        .field("weight", 4, "f32")
        .method("hp_times_two", "i32", &[], |call| {
            let doubled = hp_of(call) * 2;
            call.ret.as_deref_mut().expect("i32 return").copy_from_slice(&doubled.to_ne_bytes());
        })
        .method("scaled_hp", "i32", &["i32"], |call| {
            let factor = i32::from_ne_bytes(call.args[0].try_into().unwrap());
            let scaled = hp_of(call) * factor;
            call.ret.as_deref_mut().expect("i32 return").copy_from_slice(&scaled.to_ne_bytes());
        })
        .event("on_broken", &["i32"])
        .attribute(reflection::Attribute { name: "category".into(), value: reflection::AttributeValue::String("props".into()) })
        .register()
        .expect("register runtime crate type");

    let info = reflection::find_type(CRATE).expect("runtime type is findable");
    let fields = reflection::fields(CRATE);
    let field_names: Vec<&str> = fields.iter().map(|f| f.name.as_str()).collect();
    let method_names: Vec<String> = reflection::methods(CRATE).into_iter().map(|m| m.name).collect();
    eprintln!(
        "[reflection-rt] registered {CRATE}: size={} align={} fields={field_names:?} methods={method_names:?} type attrs={:?} hp attrs={:?}",
        info.size,
        info.align,
        reflection::type_attributes(CRATE),
        reflection::field_attributes(CRATE, "hp"),
    );
    assert_eq!((info.size, info.align), (8, 4));
    assert_eq!(field_names, ["hp", "weight"]);
    assert_eq!(method_names, ["hp_times_two", "scaled_hp"]);
    assert_eq!(reflection::type_attributes(CRATE).len(), 1);
    assert_eq!(reflection::field_attributes(CRATE, "hp")[0].value, reflection::AttributeValue::String("hp".into()));

    let mut instance = reflection::construct_default(CRATE).expect("default construct runtime type");
    let default: RuntimeCrate = bytemuck::pod_read_unaligned(instance.bytes());
    eprintln!("[reflection-rt] construct_default -> {default:?} (expected hp=40, weight=2.5)");
    assert_eq!(default, RuntimeCrate { hp: 40, weight: 2.5 });

    let doubled = reflection::invoke_method0(CRATE, "hp_times_two", instance.bytes_mut()).expect("invoke hp_times_two");
    let hook_calls = Arc::new(AtomicU32::new(0));
    let hook = {
        let hook_calls = hook_calls.clone();
        reflection::add_method_hook(CRATE, "scaled_hp", reflection::HookTiming::Before, move |call| {
            assert_eq!(call.args.len(), 1);
            hook_calls.fetch_add(1, Ordering::SeqCst);
        })
        .expect("hook runtime method")
    };
    let mut scaled = [0u8; 4];
    reflection::invoke_method(CRATE, "scaled_hp", instance.bytes_mut(), &[&3i32.to_ne_bytes()], &mut scaled)
        .expect("invoke scaled_hp(3)");
    let scaled = i32::from_ne_bytes(scaled);
    drop(hook);
    eprintln!(
        "[reflection-rt] hp_times_two() = {doubled:?}, scaled_hp(3) = {scaled}, before-hook calls = {} (expected SignedInt(80), 120, 1)",
        hook_calls.load(Ordering::SeqCst)
    );
    assert_eq!(doubled, reflection::MethodValue::SignedInt(80));
    assert_eq!(scaled, 120);
    assert_eq!(hook_calls.load(Ordering::SeqCst), 1);
    let bad_arg = reflection::invoke_method(CRATE, "scaled_hp", instance.bytes_mut(), &[&[1u8, 2]], &mut [0u8; 4]);
    let bad_ret = reflection::invoke_method(CRATE, "scaled_hp", instance.bytes_mut(), &[&3i32.to_ne_bytes()], &mut [0u8; 8]);
    assert!(bad_arg.is_err() && bad_ret.is_err(), "size mismatches must be rejected");

    let seen = Arc::new(AtomicI32::new(0));
    let subscription = {
        let seen = seen.clone();
        reflection::subscribe_event(CRATE, "on_broken", move |call| {
            seen.store(i32::from_ne_bytes(call.args[0].try_into().unwrap()), Ordering::SeqCst);
        })
        .expect("subscribe runtime event")
    };
    assert!(reflection::fire_event(CRATE, "on_broken", instance.bytes_mut(), &[&7i32.to_ne_bytes()]));
    drop(subscription);
    eprintln!("[reflection-rt] fire_event(on_broken, 7) -> listener saw {}", seen.load(Ordering::SeqCst));
    assert_eq!(seen.load(Ordering::SeqCst), 7);

    // Rejections: duplicate name, non-plain field type, unknown parameter type.
    let duplicate = reflection::TypeBuilder::for_pod::<RuntimeCrate>(CRATE).register();
    reflection::register_demo_type();
    let non_plain = reflection::TypeBuilder::new("hello.runtime.bad_field", 64, 8).field("actor", 0, "sturdy_rs.demo.actor").register();
    let bad_param = reflection::TypeBuilder::new("hello.runtime.bad_param", 4, 4).method("m", "void", &["no.such.type"], |_| {}).register();
    eprintln!("[reflection-rt] rejected: duplicate={duplicate:?} non_plain_field={non_plain:?} bad_param={bad_param:?}");
    assert!(duplicate.is_err() && non_plain.is_err() && bad_param.is_err());
    assert!(reflection::find_type("hello.runtime.bad_field").is_none());

    // A runtime type nesting another runtime type.
    reflection::TypeBuilder::new(PAIR, 16, 4)
        .field("left", 0, CRATE)
        .field("right", 8, CRATE)
        .register()
        .expect("register nested runtime type");
    let pair_fields = reflection::fields(PAIR);
    let right = pair_fields.iter().find(|f| f.name == "right").expect("right field");
    eprintln!("[reflection-rt] {PAIR}: right field type={:?} offset={} size={}", right.field_type_name, right.offset, right.size);
    assert_eq!((right.offset, right.size), (8, 8));
    assert_eq!(right.field_type_name.as_deref(), Some(CRATE));

    assert!(reflection::unregister_type(PAIR));
    assert!(reflection::unregister_type(CRATE));
    assert!(reflection::find_type(CRATE).is_none() && !reflection::unregister_type(CRATE));
    reflection::TypeBuilder::for_pod::<RuntimeCrate>(CRATE).field("hp", 0, "i32").register().expect("re-register after unregister");
    assert_eq!(reflection::fields(CRATE).len(), 1);
    assert!(reflection::unregister_type(CRATE));

    // JSON and binary serialization of a live reflected instance (a C++-registered type: the engine's
    // document layer reads fields through static accessors, which runtime-registered types lack).
    {
        const ACTOR: &str = "sturdy_rs.demo.actor";
        let mut instance = reflection::construct_default(ACTOR).expect("construct the demo actor");
        let json = instance.to_json(false).expect("to_json");
        assert!(json.starts_with('{') && json.contains("\"health\""), "json: {json}");
        assert!(instance.to_json(true).unwrap().contains('\n'), "pretty output is multi-line");
        instance.from_json(&json).expect("a document round-trips through from_json");
        assert_eq!(instance.to_json(false).unwrap(), json);
        assert!(instance.from_json("not json").is_err());
        instance.from_json("{}").expect("fields missing from the JSON are left as they were");
        assert_eq!(instance.to_json(false).unwrap(), json);
        let bytes = instance.serialize().expect("serialize");
        let mut other = reflection::construct_default(ACTOR).expect("construct another");
        assert_eq!(other.deserialize(&bytes), Ok(bytes.len()));
        assert_eq!(other.to_json(false).unwrap(), json, "binary round trip");
        assert!(other.deserialize(&bytes[..2]).is_err(), "truncated data is rejected");
    }

    // Attribute lookup, assignability and runtime enums.
    const TAGGED: &str = "hello.runtime.tagged";
    reflection::TypeBuilder::with_default(TAGGED, &RuntimeCrate { hp: 1, weight: 1.0 })
        .field("hp", 0, "i32")
        .field("weight", 4, "f32")
        .attribute(reflection::Attribute { name: "hello.tag".into(), value: reflection::AttributeValue::Bool(true) })
        .register()
        .expect("register tagged type");
    assert!(reflection::types_with_attribute("hello.tag").iter().any(|t| t == TAGGED), "attribute query finds the tagged type");
    assert!(reflection::types_with_attribute("no.such.attribute").is_empty());
    assert!(reflection::is_assignable_from(TAGGED, TAGGED), "a type is assignable from itself");
    assert!(!reflection::is_assignable_from(TAGGED, PAIR), "unrelated types are not");
    assert!(reflection::unregister_type(TAGGED));

    const MODE: &str = "hello.runtime.mode";
    reflection::register_enum(MODE, "i32", 4, 4, &[("Off", 0), ("On", 1), ("Auto", 2)]).expect("register runtime enum");
    let mode = reflection::find_enum(MODE).expect("runtime enum is findable");
    assert_eq!(mode.enumerator_count, 3);
    let values: Vec<(String, i64)> = reflection::enum_values(MODE).into_iter().map(|e| (e.name, e.value)).collect();
    assert_eq!(values, vec![("Off".to_string(), 0), ("On".to_string(), 1), ("Auto".to_string(), 2)]);
    // Repeating the same definition is a no-op; a different one is rejected rather than silently dropped.
    assert!(reflection::register_enum(MODE, "i32", 4, 4, &[("Off", 0), ("On", 1), ("Auto", 2)]).is_ok());
    assert!(reflection::register_enum(MODE, "i32", 4, 4, &[("X", 0)]).is_err());
    assert_eq!(reflection::enum_values(MODE).len(), 3, "the original enumerators are kept");
    assert!(reflection::unregister_enum(MODE) && reflection::find_enum(MODE).is_none());
    assert!(!reflection::unregister_enum(MODE), "already gone");
    eprintln!("[reflection-rt] all runtime type registration checks passed");
}

impl GameLogic for Spinner {
    fn shutdown(&mut self, _engine: &mut Engine<'_>) {
        // GPU resources must be released while the device still exists.
        if let Some(mut renderer) = self.text_renderer.take() {
            renderer.destroy();
        }
    }

    fn init(&mut self, engine: &mut Engine<'_>) -> Result<(), Error> {
        if let Some(gpu) = engine.gpu() {
            eprintln!("GPU: {} ({})", gpu.name, gpu.api_version);
        }
        demonstrate_ecs(engine);
        demonstrate_ecs_ext(engine);
        self.schedule_targets = Some(demonstrate_schedule_and_commands(engine));
        demonstrate_schedule_events(engine);
        self.ui_demo = Some(install_ui_draw_demo());
        demonstrate_images();
        demonstrate_color();
        demonstrate_shader_compiler_full();
        demonstrate_rhi_util();
        demonstrate_text();
        demonstrate_camera_rig();
        demonstrate_dock_transfer();
        demonstrate_geometry_and_threads();
        demonstrate_custom_passes();
        demonstrate_reflection();
        demonstrate_reflection_runtime_types();
        demonstrate_ecs_reflection(engine);
        demonstrate_ui_input_injection(engine);
        demonstrate_hot_reload();
        demo_async_tasks();
        demo_async_ext();
        Ok(())
    }

    fn frame(&mut self, engine: &mut Engine<'_>, frame: &Frame) -> Option<FrameDescription> {
        // `Engine::rhi` needs a live `RhiDevice`, which only exists once the renderer is up (see
        // `Engine::gpu`'s doc comment); `init` runs before that, so the RHI-extension demo runs
        // here instead, gated to the first frame where a GPU is actually reported.
        if !self.rhi_demo_done && engine.gpu().is_some() {
            demonstrate_rhi_ext(engine);
            self.rhi_demo_done = true;
        }

        if self.text_renderer.is_none() && engine.gpu().is_some() {
            self.text_renderer = Some(demonstrate_text_render(engine));
        }

        if frame.index < 3 || frame.index % 500 == 0 { eprintln!("[timings] frame.index={}", frame.index); }
        if !self.timings_checked && frame.index > 2 {
            let timings = engine.diagnostics().frame_timings(frame.window_id);
            if frame.index % 50 < 1 { eprintln!("[timings] waiting: frame {} has_data={}", frame.index, timings.has_data); }
            if timings.has_data {
                assert!(!timings.gpu_passes.is_empty() || !timings.cpu_passes.is_empty(), "a completed frame reports pass timings");
                assert!(timings.gpu_passes.iter().chain(&timings.cpu_passes).chain(&timings.cpu_stages).all(|(n, ms)| !n.is_empty() && *ms >= 0.0));
                eprintln!(
                    "[timings] {} GPU passes, {} CPU passes, {} CPU stages (first GPU pass: {:?}); async present: {}",
                    timings.gpu_passes.len(), timings.cpu_passes.len(), timings.cpu_stages.len(), timings.gpu_passes.first(),
                    engine.diagnostics().backend_allows_async_presentation()
                );
                self.timings_checked = true;
            }
        }

        if !self.window_control_done && engine.gpu().is_some() {
            demonstrate_window_control(engine);
            self.window_control_done = true;
        }

        if !self.native_window_checked && engine.gpu().is_some() {
            if let Some(handle) = engine.diagnostics().native_window_handle() {
                eprintln!("[native-window] {:?} display={:#x} window={:#x}", handle.system, handle.display, handle.window);
                assert!(handle.window != 0, "a live window reports a nonzero native handle");
                let vulkan = engine.diagnostics().native_vulkan().expect("native_access(true) exposes the Vulkan handles");
                assert!(vulkan.device != 0 && vulkan.instance != 0 && vulkan.graphics_queue != 0);
                eprintln!("[native-access] VkDevice={:#x} queue family {}", vulkan.device, vulkan.graphics_queue_family);
                self.native_window_checked = true;
            }
        }

        if !self.pipeline_demo_done && engine.gpu().is_some() {
            demonstrate_render_pipeline_ext(engine);
            self.pipeline_demo_done = true;
        }

        if !self.assets_demo_done && engine.gpu().is_some() {
            demonstrate_assets_ext(engine);
            demonstrate_gltf_ext(engine);
            self.assets_demo_done = true;
        }

        if !MAIN_THREAD_RAN.load(std::sync::atomic::Ordering::SeqCst) && self.schedule_events_frames == 5 {
            panic!("run_on_main_thread task never ran");
        }
        if !self.widgets_checked {
            self.widgets_checked = check_widget_demo(self.widgets_frames);
            self.widgets_frames += 1;
        }

        if !self.schedule_events_checked {
            self.schedule_events_checked = check_schedule_events_demo(self.schedule_events_frames);
            self.schedule_events_frames += 1;
        }

        if !self.schedule_demo_checked {
            let (target_for_destroy, target_for_components) =
                self.schedule_targets.expect("schedule_targets set by init");
            if check_schedule_and_commands_demo(
                engine,
                target_for_destroy,
                target_for_components,
                self.schedule_demo_frames_waited,
            ) {
                self.schedule_demo_checked = true;
            }
            self.schedule_demo_frames_waited += 1;
        }

        if !self.ui_demo_checked {
            let results = self.ui_demo.as_ref().expect("ui_demo set by init");
            if check_ui_draw_demo(results, self.ui_demo_frames_waited) {
                self.ui_demo_checked = true;
            }
            self.ui_demo_frames_waited += 1;
        }

        let surface = SurfaceHandle::from_window_id(frame.window_id);
        if !self.window_demo_queued {
            demonstrate_window_mutation(engine, surface);
            self.window_demo_queued = true;
        } else if !self.window_demo_checked {
            if check_window_mutation_demo(engine, surface, self.window_demo_frames_waited) {
                self.window_demo_checked = true;
            }
            self.window_demo_frames_waited += 1;
        }

        self.device_demo.step(engine, frame.index);

        let modifiers = engine.input().modifiers();
        if engine.input().key_just_pressed(Key::Enter) && modifiers.control {
            eprintln!("[input] Ctrl+Enter pressed: {modifiers:?}");
        }
        // `HELLO_STATIC_CAMERA=1` stops the demo camera spinning (handy for screenshots).
        if std::env::var_os("HELLO_STATIC_CAMERA").is_none() {
            self.yaw += 45.0 * frame.delta_seconds as f32;
        }
        if engine.input().key_just_pressed(Key::Space) {
            let scale = if engine.time_scale() == 1.0 { 0.25 } else { 1.0 };
            engine.set_time_scale(scale);
        }
        Some(
            FrameDescription::new(Camera::perspective().at([0.0, 1.0, 4.0]).euler_degrees([0.0, self.yaw, 0.0]))
                // Frame timings (`Diagnostics::frame_timings`) are collected while the debug overlay is on;
                // its on-screen text is off here.
                .render_graph({
                    let mut graph = sturdy::render::RenderGraphSettings::default();
                    graph.debug_overlay = sturdy::render::DebugOverlayRenderSettings { enabled: true, draw_text: false };
                    graph
                })
                .stages(if std::env::var_os("HELLO_NO_STAGES").is_some() { Vec::new() } else { custom_pass_stages() }),
        )
    }
}

/// Where the demo's custom fullscreen pass lives; also read by the validation in `init`.
fn tint_shader_path() -> String {
    concat!(env!("CARGO_MANIFEST_DIR"), "/assets/tint.slang").to_owned()
}

/// The standard chain with a tint pass spliced in after bloom (before tone mapping, where custom
/// passes must sit: they consume scene-linear HDR).
fn custom_pass_stages() -> Vec<sturdy::render::GraphStage> {
    use sturdy::render::{standard_stages, FullscreenEffect, GraphStage};
    let tint = FullscreenEffect::new(tint_shader_path(), "tint")
        .constants(&[1.0f32, 0.9, 0.8, 1.0])
        .label("hello tint");
    let mut stages = standard_stages();
    let after_bloom = stages.iter().position(|s| *s == GraphStage::Bloom).expect("standard chain has bloom") + 1;
    stages.insert(after_bloom, GraphStage::Fullscreen(tint));
    stages
}

/// Validates explicit stage chains (good and bad) through the engine's own graph validation.
fn demonstrate_custom_passes() {
    use sturdy::render::{standard_stages, validate_stages, GraphStage, RenderGraphSettings};
    let settings = RenderGraphSettings::default();
    validate_stages(&settings, &standard_stages()).expect("standard chain validates");
    validate_stages(&settings, &custom_pass_stages()).expect("chain with a tint pass validates");
    validate_stages(&settings, &[]).expect("empty chain is the standard graph");

    let mut after_tone_mapping = standard_stages();
    let tone = after_tone_mapping.iter().position(|s| *s == GraphStage::ToneMapping).unwrap();
    after_tone_mapping.insert(tone + 1, GraphStage::Copy("too late".into()));
    let err = validate_stages(&settings, &after_tone_mapping).expect_err("custom pass after tone mapping is rejected");
    let no_scene = validate_stages(&settings, &[GraphStage::Bloom]).expect_err("chain without a scene is rejected");
    eprintln!("[custom-passes] validation checks passed (rejections: {err:?} / {no_scene:?})");
}

fn main() -> ExitCode {
    sturdy::run(
        RuntimeConfig::new("Hello from Rust")
            .size(1280, 720)
            .vsync(VSync::Adaptive)
            .native_access(true)
            .frames_in_flight(3)
            .prefer_async_compute(true),
        Spinner {
            yaw: 0.0,
            rhi_demo_done: false,
            native_window_checked: false,
            window_control_done: false,
            timings_checked: false,
            text_renderer: None,
            pipeline_demo_done: false,
            assets_demo_done: false,
            schedule_demo_checked: false,
            schedule_events_checked: false,
            widgets_checked: false,
            widgets_frames: 0,
            schedule_events_frames: 0,
            schedule_demo_frames_waited: 0,
            schedule_targets: None,
            window_demo_queued: false,
            window_demo_checked: false,
            window_demo_frames_waited: 0,
            device_demo: DeviceDemo::default(),
            ui_demo: None,
            ui_demo_checked: false,
            ui_demo_frames_waited: 0,
        },
    )
}
