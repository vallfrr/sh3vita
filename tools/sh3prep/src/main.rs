//! sh3prep: prepares the Silent Hill 3 files (PC) for the PS Vita port.
//! Without arguments: graphical interface. Otherwise:
//! `sh3prep <game folder> <output folder> [--no-movies] [--language English|French|German|Italian|Spanish]`
mod gui;
mod i18n;
mod movie;
mod prep;

use std::path::Path;

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.is_empty() {
        gui::run();
        return;
    }
    if args.len() < 2 || args[0] == "-h" || args[0] == "--help" {
        eprintln!("usage: sh3prep <game folder> <output folder> [--no-movies] [--language NAME]\n  <game folder> contains sh3.exe and data/\n  --no-movies   do not convert the movies to MP4 (they then play at about 20 fps)\n  --language    initial game language: English (default), French, German, Italian, Spanish");
        std::process::exit(2);
    }
    let convert = !args.iter().skip(2).any(|a| a == "--no-movies");
    let language = args.iter().skip(2).position(|a| a == "--language").and_then(|i| args.get(i + 3)).map(String::as_str).unwrap_or("English");
    if !prep::LANGUAGES.contains(&language) {
        eprintln!("error: unknown language {language} (English, French, German, Italian, Spanish)");
        std::process::exit(2);
    }
    let plan = prep::check(Path::new(&args[0])).unwrap_or_else(|e| {
        eprintln!("error: {e}");
        std::process::exit(1)
    });
    println!("{} files, {:.2} GB", plan.files.len(), plan.bytes as f64 / 1e9);
    let mut last = 0;
    let (out, report) = prep::prepare(&plan, Path::new(&args[1]), convert, language, |done, _| {
        let pct = done * 100 / plan.bytes.max(1);
        if pct != last {
            last = pct;
            eprint!("\r{pct:3} %");
        }
    })
    .unwrap_or_else(|e| {
        eprintln!("\nerror: {e}");
        std::process::exit(1)
    });
    println!("\nmovies: {} decrypted, {} converted to MP4", report.decrypted, report.mp4);
    if report.ffmpeg_missing {
        println!("warning: ffmpeg was not found, the movies were not converted to MP4 (they will play at about 20 fps). Install ffmpeg and run sh3prep again.");
    }
    for f in &report.failed {
        println!("warning: {f}");
    }
    println!("done: copy the {} folder to ux0:data/ on the Vita", out.display());
}
