//! Check of the PC installation and preparation of the `sh3/` folder for the Vita.
use crate::movie;
use sha2::{Digest, Sha256};
use std::fs;
use std::io::{self, Read};
use std::path::{Path, PathBuf};

/// SHA-256 of `sh3.exe` (3,338,240 bytes): the recompiled code in the .vpk depends on it byte for byte.
pub const EXE_SHA256: &str = "a51f956bd5be21fd704c4d19cf0674a175d002081da43b1d377be83226c36e5e";
pub const EXE_NAME: &str = "sh3.exe";
/// Folder of the game that is copied as is (the movies inside it are also copied: the game checks for them).
pub const DATA_DIR: &str = "data";

#[derive(Debug)]
pub enum Error {
    MissingExe(PathBuf),
    UnsupportedExe(String),
    MissingFolder(String),
    Io { path: PathBuf, source: io::Error },
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::MissingExe(path) => write!(f, "{EXE_NAME} not found in {}", path.display()),
            Self::UnsupportedExe(sum) => write!(f, "Unsupported {EXE_NAME} (SHA-256 {sum}). The port needs the exact executable listed in the README."),
            Self::MissingFolder(name) => write!(f, "Missing folder {name}: incomplete installation."),
            Self::Io { path, source } => write!(f, "{}: {source}", path.display()),
        }
    }
}

impl std::error::Error for Error {}

fn io_error(path: &Path, source: io::Error) -> Error { Error::Io { path: path.to_owned(), source } }

#[derive(Debug)]
pub struct Plan {
    pub files: Vec<(PathBuf, PathBuf)>, // (source, relative path inside sh3/)
    pub bytes: u64,
}

/// What the movie step did, for the final report.
#[derive(Debug, Default, Clone)]
pub struct MovieReport {
    pub decrypted: usize,
    pub mp4: usize,
    pub ffmpeg_missing: bool,
    pub failed: Vec<String>,
}

pub fn sha256_file(p: &Path) -> io::Result<String> {
    let mut f = fs::File::open(p)?;
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    loop {
        let n = f.read(&mut buf)?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
    }
    Ok(h.finalize().iter().map(|b| format!("{b:02x}")).collect())
}

/// Checks the game folder and lists the files to copy.
pub fn check(game: &Path) -> Result<Plan, Error> {
    let exe = game.join(EXE_NAME);
    if !exe.is_file() {
        return Err(Error::MissingExe(game.to_owned()));
    }
    let sum = sha256_file(&exe).map_err(|e| io_error(&exe, e))?;
    if sum != EXE_SHA256 {
        return Err(Error::UnsupportedExe(sum));
    }
    let data = game.join(DATA_DIR);
    if !data.is_dir() {
        return Err(Error::MissingFolder(DATA_DIR.to_owned()));
    }
    for name in movie::NAMES {
        if !data.join("movie").join(format!("{name}.000")).is_file() {
            return Err(Error::MissingFolder(format!("{DATA_DIR}/movie/{name}.000")));
        }
    }
    let mut plan = Plan { files: vec![(exe.clone(), PathBuf::from(EXE_NAME))], bytes: 0 };
    walk(&data, Path::new(DATA_DIR), &mut plan.files).map_err(|e| io_error(&data, e))?;
    plan.bytes = plan.files.iter().map(|(s, _)| fs::metadata(s).map(|m| m.len()).unwrap_or(0)).sum();
    Ok(plan)
}

fn walk(dir: &Path, rel: &Path, out: &mut Vec<(PathBuf, PathBuf)>) -> io::Result<()> {
    let mut entries: Vec<_> = fs::read_dir(dir)?.collect::<Result<_, _>>()?;
    entries.sort_by_key(|e| e.file_name());
    for e in entries {
        let p = e.path();
        let r = rel.join(e.file_name());
        if p.is_dir() {
            walk(&p, &r, out)?;
        } else {
            out.push((p, r));
        }
    }
    Ok(())
}

/// Copies the files into `<out>/sh3/` (files already copied at the right size are skipped), decrypts the movies into
/// `<out>/sh3/movie/` (and converts them to MP4 when `convert_movies` is set and ffmpeg is installed), then writes
/// `sh3vita.json`. `progress(bytes done, current file)`.
pub fn prepare(
    plan: &Plan,
    out: &Path,
    convert_movies: bool,
    mut progress: impl FnMut(u64, &Path),
) -> Result<(PathBuf, MovieReport), Error> {
    let dst_root = out.join("sh3");
    let mut done = 0u64;
    for (src, rel) in &plan.files {
        let dst = dst_root.join(rel);
        let size = fs::metadata(src).map_err(|e| io_error(src, e))?.len();
        progress(done, rel);
        let same = fs::metadata(&dst).map(|m| m.len() == size).unwrap_or(false)
            && (rel != Path::new(EXE_NAME) || sha256_file(&dst).map(|s| s == EXE_SHA256).unwrap_or(false));
        if !same {
            if let Some(parent) = dst.parent() {
                fs::create_dir_all(parent).map_err(|e| io_error(parent, e))?;
            }
            fs::copy(src, &dst).map_err(|e| io_error(&dst, e))?;
        }
        done += size;
    }
    // Movies: decrypted .mpg (software decoder) and, when possible, .mp4 (hardware decoder, 30 fps).
    let movie_dir = dst_root.join("movie");
    fs::create_dir_all(&movie_dir).map_err(|e| io_error(&movie_dir, e))?;
    let mut report = MovieReport::default();
    let ffmpeg = convert_movies && movie::ffmpeg_available();
    report.ffmpeg_missing = convert_movies && !ffmpeg;
    for name in movie::NAMES {
        let source = dst_root.join(DATA_DIR).join("movie").join(format!("{name}.000"));
        let mpg = movie_dir.join(format!("{name}.mpg"));
        let mp4 = movie_dir.join(format!("{name}.mp4"));
        progress(done, Path::new(&format!("movie/{name}.mpg")));
        if !mpg.is_file() || fs::metadata(&mpg).map(|m| m.len()).unwrap_or(0) + 0x8000 != fs::metadata(&source).map(|m| m.len()).unwrap_or(0) {
            if let Err(e) = movie::demangle(&source, &mpg) {
                let _ = fs::remove_file(&mpg);
                report.failed.push(format!("{name}: {e}"));
                continue;
            }
        }
        report.decrypted += 1;
        if ffmpeg {
            progress(done, Path::new(&format!("movie/{name}.mp4")));
            if !mp4.is_file() {
                if let Err(e) = movie::to_mp4(&mpg, &mp4) {
                    report.failed.push(format!("{name}.mp4: {e}"));
                    continue;
                }
            }
            report.mp4 += 1;
        }
    }
    let manifest = format!(
        "{{\n  \"format\": 1,\n  \"exe_sha256\": \"{EXE_SHA256}\",\n  \"files\": {},\n  \"bytes\": {},\n  \"movies_decrypted\": {},\n  \"movies_mp4\": {},\n  \"created_by\": \"sh3prep {}\"\n}}\n",
        plan.files.len(),
        plan.bytes,
        report.decrypted,
        report.mp4,
        env!("CARGO_PKG_VERSION")
    );
    let manifest_path = dst_root.join("sh3vita.json");
    fs::write(&manifest_path, manifest).map_err(|e| io_error(&manifest_path, e))?;
    progress(done, Path::new("sh3vita.json"));
    Ok((dst_root, report))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn refuses_wrong_exe() {
        let tmp = std::env::temp_dir().join(format!("sh3prep-test-{}", std::process::id()));
        let game = tmp.join("game");
        fs::create_dir_all(&game).unwrap();
        fs::write(game.join(EXE_NAME), b"not the right exe").unwrap();
        assert!(matches!(check(&game), Err(Error::UnsupportedExe(_))));
        fs::remove_dir_all(&tmp).unwrap();
    }

    #[test]
    fn refuses_missing_exe() {
        let tmp = std::env::temp_dir().join(format!("sh3prep-test2-{}", std::process::id()));
        fs::create_dir_all(&tmp).unwrap();
        assert!(matches!(check(&tmp), Err(Error::MissingExe(_))));
        fs::remove_dir_all(&tmp).unwrap();
    }

    /// With SH3PREP_TEST_GAME (a real, supported installation) the plan must list the exe and every data file.
    #[test]
    fn plans_real_installation_when_provided() {
        if let Some(game) = std::env::var_os("SH3PREP_TEST_GAME") {
            let plan = check(Path::new(&game)).unwrap();
            assert_eq!(plan.files[0].1, PathBuf::from(EXE_NAME));
            assert!(plan.files.iter().any(|(_, r)| r == Path::new("data/movie/op.000")));
            assert!(plan.bytes > 1_000_000_000);
        }
    }
}
