//! The seven PC movies (`data/movie/*.000`) are MPEG program streams obfuscated with a 32-bit LCG keystream.
//!
//! Scheme (found by reading the game's own movie reader, FUN_00405680 in sh3.exe, and cross-checked against the
//! a published description of the container format):
//!  * the file starts with a 0x8000-byte block whose plaintext is all zeros; four bytes of its ciphertext, at fixed
//!    offsets, give the seed: `f[0x2dde] | f[0x3d40]<<8 | f[0x2a5c]<<16 | f[0x55e2]<<24`;
//!  * LCG: `state = state * 0x5d588b65 + 1` (MSVC `rand()` multiplier);
//!  * `base` = the state after 0x2000 steps from the seed;
//!  * the data region (from 0x8000) uses a keystream that is periodic every 0x1000 bytes: the state restarts at
//!    `base` for each 4096-byte chunk and every 32-bit little-endian word is XORed with the next state.
//! The result is an MPEG-2 program stream (MPEG-1 video + MP2 audio) that the Vita plays directly (software
//! decoder) or that ffmpeg converts to MP4 for the Vita's hardware decoder.
use std::fs::{self, File};
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::OnceLock;

const MULT: u32 = 0x5D58_8B65;
pub const HEADER: usize = 0x8000;
const CHUNK: usize = 0x1000;

/// Names of the movies the port knows (`data/movie/<name>.000`).
pub const NAMES: [&str; 7] = ["op", "ed_b", "ed_u", "ed_n", "henshin", "huxx", "muxx"];

fn base_state(header: &[u8]) -> u32 {
    let seed = header[0x2DDE] as u32
        | (header[0x3D40] as u32) << 8
        | (header[0x2A5C] as u32) << 16
        | (header[0x55E2] as u32) << 24;
    let mut state = seed;
    for _ in 0..HEADER / 4 {
        state = state.wrapping_mul(MULT).wrapping_add(1);
    }
    state
}

fn decrypt_chunk(buf: &mut [u8], base: u32) {
    let mut state = base;
    let words = buf.len() - buf.len() % 4;
    for word in buf[..words].chunks_exact_mut(4) {
        state = state.wrapping_mul(MULT).wrapping_add(1);
        let plain = u32::from_le_bytes([word[0], word[1], word[2], word[3]]) ^ state;
        word.copy_from_slice(&plain.to_le_bytes());
    }
}

/// Writes the first `HEADER` bytes of `src` (.000) to `dst`: what the game reads of a movie whose `.mp4` the port plays.
pub fn write_header(src: &Path, dst: &Path) -> io::Result<()> {
    let mut head = vec![0u8; HEADER];
    File::open(src)?.read_exact(&mut head)?;
    fs::write(dst, head)
}

/// Decrypts `src` (.000) into `dst` (.mpg). The output is `src.len() - 0x8000` bytes.
pub fn demangle(src: &Path, dst: &Path) -> io::Result<u64> {
    let mut input = File::open(src)?;
    let mut header = vec![0u8; HEADER];
    input.read_exact(&mut header).map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "movie file too small"))?;
    let base = base_state(&header);
    let mut output = io::BufWriter::new(File::create(dst)?);
    let mut buf = vec![0u8; CHUNK];
    let mut total = 0u64;
    let mut first = true;
    loop {
        let mut filled = 0;
        while filled < CHUNK {
            let n = input.read(&mut buf[filled..])?;
            if n == 0 {
                break;
            }
            filled += n;
        }
        if filled == 0 {
            break;
        }
        decrypt_chunk(&mut buf[..filled], base);
        if first {
            // Every pack of an MPEG-2 program stream starts with 00 00 01 BA: refuse anything else.
            if buf[..4] != [0, 0, 1, 0xBA] {
                return Err(io::Error::new(io::ErrorKind::InvalidData, "not a Silent Hill 3 movie (bad pack header)"));
            }
            first = false;
        }
        output.write_all(&buf[..filled])?;
        total += filled as u64;
    }
    output.flush()?;
    Ok(total)
}

// ---- FFmpeg: embedded build (release binaries), SH3PREP_FFMPEG, or the system's ffmpeg ----

#[cfg(embedded_ffmpeg)]
static EMBEDDED_FFMPEG: &[u8] = include_bytes!("../ffmpeg/ffmpeg.gz");

fn command(program: &Path) -> Command {
    #[allow(unused_mut)]
    let mut c = Command::new(program);
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        c.creation_flags(0x0800_0000); // CREATE_NO_WINDOW: no console flashing from the GUI
    }
    c
}

fn runs(program: &Path) -> bool {
    command(program).arg("-version").stdout(Stdio::null()).stderr(Stdio::null()).status().map(|s| s.success()).unwrap_or(false)
}

/// Extracts the embedded ffmpeg (once, into the system temp folder) and returns its path.
#[cfg(embedded_ffmpeg)]
fn embedded_ffmpeg() -> Option<PathBuf> {
    use sha2::{Digest, Sha256};
    let tag: String = Sha256::digest(EMBEDDED_FFMPEG).iter().take(4).map(|b| format!("{b:02x}")).collect();
    let dir = std::env::temp_dir().join(format!("sh3prep-ffmpeg-{tag}"));
    let exe = dir.join(if cfg!(windows) { "ffmpeg.exe" } else { "ffmpeg" });
    if exe.is_file() && runs(&exe) {
        return Some(exe);
    }
    fs::create_dir_all(&dir).ok()?;
    let partial = dir.join("ffmpeg.partial");
    let mut out = File::create(&partial).ok()?;
    io::copy(&mut flate2::read::GzDecoder::new(EMBEDDED_FFMPEG), &mut out).ok()?;
    out.flush().ok()?;
    drop(out);
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(&partial, fs::Permissions::from_mode(0o755)).ok()?;
    }
    let _ = fs::remove_file(&exe);
    fs::rename(&partial, &exe).ok()?;
    runs(&exe).then_some(exe)
}

#[cfg(not(embedded_ffmpeg))]
fn embedded_ffmpeg() -> Option<PathBuf> { None }

fn find_ffmpeg() -> Option<PathBuf> {
    if let Some(path) = std::env::var_os("SH3PREP_FFMPEG").map(PathBuf::from) {
        if runs(&path) {
            return Some(path);
        }
    }
    if let Some(path) = embedded_ffmpeg() {
        return Some(path);
    }
    let system = PathBuf::from("ffmpeg");
    runs(&system).then_some(system)
}

/// The ffmpeg to use (embedded build first), if any can be run.
pub fn ffmpeg() -> Option<&'static Path> {
    static FFMPEG: OnceLock<Option<PathBuf>> = OnceLock::new();
    FFMPEG.get_or_init(find_ffmpeg).as_deref()
}

/// True if an ffmpeg can be run.
pub fn ffmpeg_available() -> bool {
    ffmpeg().is_some()
}

/// Converts the decrypted MPEG to the H.264/AAC MP4 the Vita's hardware decoder plays at 30 fps
/// (settings validated on a real console: baseline profile level 3.1, CRF 18, AAC-LC 48 kHz stereo).
pub fn to_mp4(mpg: &Path, mp4: &Path) -> io::Result<()> {
    let ffmpeg = ffmpeg().ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, "ffmpeg not available"))?;
    let output = command(ffmpeg)
        .args(["-nostdin", "-v", "error", "-y", "-i"])
        .arg(mpg)
        .args(["-map", "0:v:0", "-map", "0:a:0?", "-c:v", "libx264", "-profile:v", "baseline", "-level", "3.1"])
        .args(["-pix_fmt", "yuv420p", "-crf", "18", "-c:a", "aac", "-ar", "48000", "-ac", "2", "-movflags", "+faststart"])
        .arg(mp4)
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .output()?;
    if !output.status.success() {
        let _ = std::fs::remove_file(mp4);
        return Err(io::Error::new(io::ErrorKind::Other, format!("ffmpeg failed: {}", String::from_utf8_lossy(&output.stderr).trim())));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Builds a fake movie: zero padding block encrypted with a chosen seed, then two packs.
    fn fake_movie(plain_data: &[u8], seed: u32) -> Vec<u8> {
        let mut file = vec![0u8; HEADER];
        // Seed bytes live at fixed offsets of the (otherwise zero) header.
        file[0x2DDE] = seed as u8;
        file[0x3D40] = (seed >> 8) as u8;
        file[0x2A5C] = (seed >> 16) as u8;
        file[0x55E2] = (seed >> 24) as u8;
        let base = base_state(&file);
        let mut data = plain_data.to_vec();
        for chunk in data.chunks_mut(CHUNK) {
            decrypt_chunk(chunk, base); // XOR is its own inverse
        }
        file.extend_from_slice(&data);
        file
    }

    #[test]
    fn round_trips_synthetic_movie() {
        let mut plain = vec![0u8; 3 * 2048 + 100];
        for pack in 0..3 {
            plain[pack * 2048..pack * 2048 + 4].copy_from_slice(&[0, 0, 1, 0xBA]);
        }
        for (i, b) in plain.iter_mut().enumerate().skip(4) {
            if i % 2048 >= 4 {
                *b = (i * 31 % 251) as u8;
            }
        }
        let tmp = std::env::temp_dir().join(format!("sh3prep-movie-{}", std::process::id()));
        std::fs::create_dir_all(&tmp).unwrap();
        let src = tmp.join("x.000");
        let dst = tmp.join("x.mpg");
        std::fs::write(&src, fake_movie(&plain, 0x1234_5678)).unwrap();
        assert_eq!(demangle(&src, &dst).unwrap(), plain.len() as u64);
        assert_eq!(std::fs::read(&dst).unwrap(), plain);
        std::fs::remove_dir_all(&tmp).unwrap();
    }

    #[test]
    fn rejects_garbage() {
        let tmp = std::env::temp_dir().join(format!("sh3prep-bad-{}", std::process::id()));
        std::fs::create_dir_all(&tmp).unwrap();
        let src = tmp.join("x.000");
        std::fs::write(&src, vec![7u8; HEADER + 8192]).unwrap();
        assert!(demangle(&src, &tmp.join("x.mpg")).is_err());
        std::fs::remove_dir_all(&tmp).unwrap();
    }

    /// The release builds embed an ffmpeg: it must offer the two encoders the conversion needs.
    #[cfg(embedded_ffmpeg)]
    #[test]
    fn embedded_ffmpeg_has_the_needed_encoders() {
        let path = embedded_ffmpeg().expect("the embedded ffmpeg must run");
        let out = command(&path).args(["-hide_banner", "-encoders"]).output().unwrap();
        let text = String::from_utf8_lossy(&out.stdout);
        assert!(text.contains("libx264"), "no libx264 encoder");
        assert!(text.contains(" aac "), "no aac encoder");
    }

    /// With SH3PREP_TEST_MOVIE (a real .000) and SH3PREP_TEST_MPG (a known-good decrypted copy) the output must match.
    #[test]
    fn matches_reference_when_provided() {
        if let (Some(movie), Some(reference)) = (std::env::var_os("SH3PREP_TEST_MOVIE"), std::env::var_os("SH3PREP_TEST_MPG")) {
            let out = std::env::temp_dir().join(format!("sh3prep-ref-{}.mpg", std::process::id()));
            demangle(Path::new(&movie), &out).unwrap();
            assert!(std::fs::read(&out).unwrap() == std::fs::read(&reference).unwrap(), "decrypted movie differs from the reference");
            std::fs::remove_file(out).unwrap();
        }
    }
}
