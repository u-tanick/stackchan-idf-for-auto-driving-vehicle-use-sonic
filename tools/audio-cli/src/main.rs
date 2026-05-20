// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// BLE audio streaming CLI for stackchan-idf. Implements the same protocol
// the browser side of tools/settings.html does: scan for a `Stackchan-*`
// peripheral, run an X25519 + AES-256-GCM handshake on the KeyExchange
// chr, then stream AAC ADTS bytes on the AudioData chr framed by the
// AudioControl chr's `begin` / `end` JSON.
//
// Purpose: bisect "is the BLE disconnect from Chrome or from the device"
// by removing the browser entirely. Same pacing knobs (chunk size, rate
// limit, with-response cadence) as the browser, controllable from the
// command line.

use std::time::{Duration, Instant};

use anyhow::{anyhow, Context, Result};
use btleplug::api::{
    Central, Manager as _, Peripheral as _, ScanFilter, WriteType,
};
use btleplug::platform::{Manager, Peripheral};
use clap::Parser;
use uuid::Uuid;

use aes_gcm::{
    aead::{Aead, AeadCore, KeyInit},
    Aes256Gcm,
};
use hkdf::Hkdf;
use rand_core::{OsRng, RngCore};
use sha2::Sha256;
use x25519_dalek::{EphemeralSecret, PublicKey};

// UUIDs straight from components/config_service/gatt_settings.cpp.
const SVC_UUID: Uuid = Uuid::from_u128(0xe3f0a000_7b1c_4d2a_9e6f_2c5a8d4b1f00);
const CHR_KX: Uuid = Uuid::from_u128(0xe3f0a006_7b1c_4d2a_9e6f_2c5a8d4b1f00);
const CHR_AUDIO_CTRL: Uuid = Uuid::from_u128(0xe3f0a00e_7b1c_4d2a_9e6f_2c5a8d4b1f00);
const CHR_AUDIO_DATA: Uuid = Uuid::from_u128(0xe3f0a00f_7b1c_4d2a_9e6f_2c5a8d4b1f00);
const CHR_AUDIO_CREDIT: Uuid = Uuid::from_u128(0xe3f0a010_7b1c_4d2a_9e6f_2c5a8d4b1f00);

#[derive(Parser, Debug)]
#[command(version, about, long_about = None)]
struct Args {
    /// BLE local-name prefix to connect to (e.g. "Stackchan-E2604E").
    /// Just "Stackchan-" matches the first device that comes up.
    #[arg(long, default_value = "Stackchan-")]
    device: String,

    /// ADTS AAC file to stream. Convert any source with:
    ///   ffmpeg -i in.mp3 -c:a aac -b:a 96k -ar 48000 -ac 1 -f adts out.aac
    #[arg(long)]
    file: String,

    /// Per-write plaintext size (encryption adds 28 bytes). The BLE
    /// attribute cap is 512 — leave headroom.
    #[arg(long, default_value_t = 250)]
    chunk_size: usize,

    /// Optional throughput ceiling in KiB/s, applied *on top of* the
    /// credit-based flow control. 0 (the default) means no artificial cap —
    /// the AudioCredit characteristic alone paces the stream to the device's
    /// playback rate. Set a value only to debug a slower link.
    #[arg(long, default_value_t = 0.0)]
    rate_kbps: f64,

    /// Every Nth write uses with-response (sync point). Set 1 to make
    /// every write with-response (slower, more deterministic).
    #[arg(long, default_value_t = 16)]
    flush_every: usize,

    /// Sample rate to report to the device on `begin`. The device's AAC
    /// decoder auto-detects from ADTS headers so this is mostly cosmetic,
    /// but the begin JSON includes it for consistency with the browser.
    #[arg(long, default_value_t = 48000)]
    sample_rate: u32,

    /// Connect and run handshake, then exit without streaming. Useful for
    /// link-layer debugging.
    #[arg(long, default_value_t = false)]
    handshake_only: bool,
}

async fn scan_for_device(manager: &Manager, name_prefix: &str) -> Result<Peripheral> {
    let adapters = manager.adapters().await?;
    let adapter = adapters
        .into_iter()
        .next()
        .ok_or_else(|| anyhow!("no BLE adapter found"))?;

    adapter
        .start_scan(ScanFilter {
            services: vec![SVC_UUID],
        })
        .await?;
    println!("scanning for {}*...", name_prefix);

    let deadline = Instant::now() + Duration::from_secs(20);
    loop {
        for p in adapter.peripherals().await? {
            if let Some(props) = p.properties().await? {
                if let Some(name) = props.local_name {
                    if name.starts_with(name_prefix) {
                        println!("found {} (addr {})", name, p.address());
                        let _ = adapter.stop_scan().await;
                        return Ok(p);
                    }
                }
            }
        }
        if Instant::now() >= deadline {
            let _ = adapter.stop_scan().await;
            return Err(anyhow!(
                "scan timed out — no peripheral matched '{}*'",
                name_prefix
            ));
        }
        tokio::time::sleep(Duration::from_millis(300)).await;
    }
}

async fn run_handshake(peripheral: &Peripheral) -> Result<Aes256Gcm> {
    let chars = peripheral.characteristics();
    let kx = chars
        .iter()
        .find(|c| c.uuid == CHR_KX)
        .ok_or_else(|| anyhow!("KeyExchange chr (e3f0a006-…) not found"))?;

    // 1. Read the device's ephemeral X25519 public key.
    let device_pub_bytes = peripheral.read(kx).await?;
    if device_pub_bytes.len() != 32 {
        return Err(anyhow!(
            "expected 32-byte device pubkey, got {}",
            device_pub_bytes.len()
        ));
    }
    let mut buf = [0u8; 32];
    buf.copy_from_slice(&device_pub_bytes);
    let device_pub = PublicKey::from(buf);

    // 2. Generate our own keypair.
    let our_priv = EphemeralSecret::random_from_rng(OsRng);
    let our_pub = PublicKey::from(&our_priv);

    // 3. ECDH → 32-byte shared secret.
    let shared = our_priv.diffie_hellman(&device_pub);

    // 4. HKDF-SHA256(salt = empty, info = "stackchan-config-v1") → 32-byte AES-256 key.
    //    Matches components/config_service/crypto.cpp exactly.
    let hk = Hkdf::<Sha256>::new(None, shared.as_bytes());
    let mut aes_key = [0u8; 32];
    hk.expand(b"stackchan-config-v1", &mut aes_key)
        .map_err(|e| anyhow!("HKDF expand: {e}"))?;

    // 5. Send our public key back. The device's complete_handshake fires
    //    here and the session is live.
    peripheral
        .write(kx, our_pub.as_bytes(), WriteType::WithResponse)
        .await
        .context("write our pubkey to KeyExchange")?;

    Ok(Aes256Gcm::new_from_slice(&aes_key).expect("32-byte key"))
}

// Read the device's current audio-buffer free space (uint32 LE) from the
// AudioCredit characteristic.
async fn read_credit(
    peripheral: &Peripheral,
    credit_chr: &btleplug::api::Characteristic,
) -> Result<u32> {
    let v = peripheral.read(credit_chr).await?;
    let bytes = [
        *v.first().unwrap_or(&0),
        *v.get(1).unwrap_or(&0),
        *v.get(2).unwrap_or(&0),
        *v.get(3).unwrap_or(&0),
    ];
    Ok(u32::from_le_bytes(bytes))
}

fn encrypt(cipher: &Aes256Gcm, plaintext: &[u8]) -> Vec<u8> {
    // Wire format: [12B nonce][ciphertext][16B GCM tag].
    let nonce = Aes256Gcm::generate_nonce(&mut OsRng);
    let ct = cipher.encrypt(&nonce, plaintext).expect("AES-GCM encrypt");
    let mut wire = Vec::with_capacity(nonce.len() + ct.len());
    wire.extend_from_slice(&nonce);
    wire.extend_from_slice(&ct);
    wire
}

#[tokio::main]
async fn main() -> Result<()> {
    let args = Args::parse();

    let file_bytes = std::fs::read(&args.file)
        .with_context(|| format!("reading audio file {}", args.file))?;
    println!("audio file: {} ({} bytes)", args.file, file_bytes.len());

    // Quick sanity check on the input — the ESP-side decoder expects ADTS
    // sync (0xFF 0xF{0..F}) at the very first byte. A wrong byte here is
    // almost always "ffmpeg wrote a raw AAC or MP4 container instead of
    // ADTS"; warn loudly rather than streaming garbage.
    if file_bytes.len() < 2 || file_bytes[0] != 0xFF || (file_bytes[1] & 0xF0) != 0xF0 {
        let head: String = file_bytes
            .iter()
            .take(8)
            .map(|b| format!("{:02x}", b))
            .collect::<Vec<_>>()
            .join(" ");
        eprintln!(
            "WARNING: file does not start with an ADTS sync word (got {}).\n\
             Re-encode with:  ffmpeg -i <in> -c:a aac -profile:a aac_low \\\n\
             \t-b:a 96k -ar 48000 -ac 1 -f adts <out>.aac",
            head
        );
    } else {
        let head: String = file_bytes
            .iter()
            .take(32)
            .map(|b| format!("{:02x}", b))
            .collect::<Vec<_>>()
            .join(" ");
        println!("first 32 bytes: {}", head);
    }

    let manager = Manager::new().await?;
    let peripheral = scan_for_device(&manager, &args.device).await?;

    println!("connecting...");
    peripheral.connect().await.context("BLE connect")?;
    peripheral
        .discover_services()
        .await
        .context("discover services")?;

    let cipher = run_handshake(&peripheral)
        .await
        .context("X25519 + AES-GCM handshake")?;
    println!("handshake OK (X25519 + AES-256-GCM)");

    if args.handshake_only {
        let _ = peripheral.disconnect().await;
        return Ok(());
    }

    let chars = peripheral.characteristics();
    let ctrl = chars
        .iter()
        .find(|c| c.uuid == CHR_AUDIO_CTRL)
        .ok_or_else(|| anyhow!("AudioControl chr not found — firmware too old?"))?
        .clone();
    let data = chars
        .iter()
        .find(|c| c.uuid == CHR_AUDIO_DATA)
        .ok_or_else(|| anyhow!("AudioData chr not found"))?
        .clone();
    let credit_chr = chars
        .iter()
        .find(|c| c.uuid == CHR_AUDIO_CREDIT)
        .ok_or_else(|| anyhow!("AudioCredit chr not found — firmware too old?"))?
        .clone();

    // Suppress the unused-warning for the cipher when audio chrs go
    // plaintext. The handshake itself is still required (the device
    // expects an established session for any settings chr write), even
    // though audio_ctrl + audio_data take the plain bypass on the device.
    let _ = &cipher;

    // begin — plaintext. Audio payloads are ephemeral playback samples,
    // not credentials, so the AES-GCM overhead (12 + 16 wire bytes per
    // chunk + per-chunk CPU on both ends) buys us no security benefit
    // worth the BLE bandwidth cost.
    let begin_json = format!(
        r#"{{"op":"begin","codec":"aac","sample_rate":{},"channels":1}}"#,
        args.sample_rate
    );
    peripheral
        .write(&ctrl, begin_json.as_bytes(), WriteType::WithResponse)
        .await
        .context("send begin")?;
    println!("begin sent — streaming {} bytes...", file_bytes.len());

    // stream
    let target_bps = args.rate_kbps * 1024.0;
    let start = Instant::now();
    let mut bytes_sent: u64 = 0;
    let mut chunks_sent: usize = 0;
    let total = file_bytes.len();

    // Credit-based flow control. The device exposes its current buffer free
    // space (uint32 LE) on the AudioCredit characteristic. We never have
    // more bytes outstanding than we last read free: read credit, send up to
    // that many no-response writes, read again. Free space only grows
    // between reads (single producer; the device drains as it plays), so the
    // value is a safe lower bound and we never overflow. When the device
    // buffer is full the read returns ~0 and we poll until playback frees
    // space — that's what paces the whole stream to the playback rate.
    let mut credit: u32 = read_credit(&peripheral, &credit_chr).await?;
    let mut credit_reads: usize = 1;

    // Ctrl-C → send abort + disconnect cleanly. Spawn a watcher.
    let ctrl_c = tokio::signal::ctrl_c();
    tokio::pin!(ctrl_c);

    'stream: for chunk in file_bytes.chunks(args.chunk_size) {
        // Non-blocking check for ctrl-c without awaiting.
        if let Ok(()) = tokio::time::timeout(Duration::from_millis(0), &mut ctrl_c).await.unwrap_or(Err(std::io::Error::new(std::io::ErrorKind::Other, ""))) {
            println!("\nctrl-c — aborting");
            let _ = peripheral
                .write(&ctrl, br#"{"op":"abort"}"#, WriteType::WithResponse)
                .await;
            break 'stream;
        }

        // Block until the device has room for this chunk. Refresh the credit
        // window whenever our running estimate dips below the chunk size;
        // poll-sleep while the device reports full.
        let need = chunk.len() as u32;
        while credit < need {
            credit = read_credit(&peripheral, &credit_chr).await?;
            credit_reads += 1;
            if credit < need {
                tokio::time::sleep(Duration::from_millis(15)).await;
            }
        }

        // Plaintext audio bytes (see begin write above for the rationale).
        // No-response writes for throughput; an occasional with-response
        // write keeps the host adapter's TX queue from ballooning.
        let use_no_resp = args.flush_every > 1 && (chunks_sent + 1) % args.flush_every != 0;
        let wtype = if use_no_resp {
            WriteType::WithoutResponse
        } else {
            WriteType::WithResponse
        };
        if let Err(e) = peripheral.write(&data, chunk, wtype).await {
            eprintln!("\nwrite failed at chunk {}: {}", chunks_sent, e);
            return Err(e.into());
        }

        chunks_sent += 1;
        bytes_sent += chunk.len() as u64;
        credit = credit.saturating_sub(need);

        // Optional throughput ceiling on top of credit flow control.
        if target_bps > 0.0 {
            let elapsed_s = start.elapsed().as_secs_f64();
            let expected_s = bytes_sent as f64 / target_bps;
            let lag_ms = (expected_s - elapsed_s) * 1000.0;
            if lag_ms > 5.0 {
                tokio::time::sleep(Duration::from_millis(lag_ms as u64)).await;
            }
        }

        if chunks_sent % 32 == 0 || bytes_sent as usize == total {
            let rate = bytes_sent as f64 / start.elapsed().as_secs_f64() / 1024.0;
            let pct = (bytes_sent as f64) * 100.0 / (total as f64);
            print!(
                "\r  {:>5.1}%  {:>5} chunks  {:>7} / {:>7} B  {:>5.1} KiB/s  credit={:>6}B reads={}   ",
                pct, chunks_sent, bytes_sent, total, rate, credit, credit_reads
            );
            use std::io::Write as _;
            let _ = std::io::stdout().flush();
        }
    }
    println!();

    // end
    peripheral
        .write(&ctrl, br#"{"op":"end"}"#, WriteType::WithResponse)
        .await
        .context("send end")?;
    let total_s = start.elapsed().as_secs_f64();
    println!(
        "end sent — {} bytes in {:.1}s ({:.1} KiB/s, {} credit reads)",
        bytes_sent,
        total_s,
        bytes_sent as f64 / total_s / 1024.0,
        credit_reads
    );

    // Give the device a moment to finish its receive phase and kick off
    // playback before we tear the link down.
    tokio::time::sleep(Duration::from_secs(2)).await;
    let _ = peripheral.disconnect().await;
    Ok(())
}

// Keep RngCore in the symbol table so cargo doesn't warn about an unused
// import when we tweak feature flags. (encrypt() uses OsRng through aead's
// generate_nonce which depends on it transitively.)
#[allow(dead_code)]
fn _force_rngcore_link<R: RngCore>(_: R) {}
