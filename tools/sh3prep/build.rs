// An FFmpeg build can be embedded in sh3prep: the release workflow puts a pinned, hash-checked gzip of a static
// ffmpeg at `ffmpeg/ffmpeg.gz` before building. Without that file (a plain local build) sh3prep uses the ffmpeg
// installed on the computer, if any.
fn main() {
    println!("cargo:rustc-check-cfg=cfg(embedded_ffmpeg)");
    println!("cargo:rerun-if-changed=ffmpeg/ffmpeg.gz");
    println!("cargo:rerun-if-changed=build.rs");
    if std::path::Path::new("ffmpeg/ffmpeg.gz").is_file() {
        println!("cargo:rustc-cfg=embedded_ffmpeg");
    }
}
