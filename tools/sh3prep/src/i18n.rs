//! Interface texts (English, French). The game itself is translated in-game, not here.
use crate::prep::Error;

pub const NAMES: [&str; 2] = ["English", "Français"];

pub struct Text {
    pub title: &'static str,
    pub intro: &'static str,
    pub language: &'static str,
    pub game: &'static str,
    pub game_hint: &'static str,
    pub output: &'static str,
    pub output_hint: &'static str,
    pub browse: &'static str,
    pub movies: &'static str,
    pub movies_hint: &'static str,
    pub prepare: &'static str,
    pub ready: &'static str,
    pub checking: &'static str,
    pub copying: &'static str,
    pub files: &'static str,
    pub complete: &'static str,
    pub transfer: &'static str,
    pub failed: &'static str,
    pub no_ffmpeg: &'static str,
    pub footer: &'static str,
    pub missing_exe: &'static str,
    pub bad_exe: &'static str,
    pub missing_folder: &'static str,
}

pub const TEXT: [Text; 2] = [
    Text {
        title: "Prepare Silent Hill 3 for the Vita",
        intro: "Copies your own game files, decrypts the movies and converts them for the Vita's video decoder. No game file is ever sent anywhere.",
        language: "Interface language",
        game: "Game folder",
        game_hint: "The folder that contains sh3.exe and the data folder.",
        output: "Output folder",
        output_hint: "A separate folder: an sh3 folder is created inside it, about 5 GB.",
        browse: "Browse…",
        movies: "Convert the movies to MP4 (recommended)",
        movies_hint: "Uses the ffmpeg built into sh3prep (or the one installed on this computer). Without it the movies play at about 20 fps.",
        prepare: "Prepare the files",
        ready: "Choose the two folders, then prepare.",
        checking: "Checking the game files…",
        copying: "Preparing…",
        files: "files",
        complete: "Preparation complete.",
        transfer: "Copy the sh3 folder to ux0:data/ on the Vita (USB is much faster than FTP):",
        failed: "Preparation failed",
        no_ffmpeg: "ffmpeg was not found: the movies were not converted to MP4 and will play at about 20 fps. Install ffmpeg and prepare again.",
        footer: "sh3prep is part of sh3vita (GPL-3.0). Silent Hill is a trademark of Konami; this project is not affiliated with Konami.",
        missing_exe: "sh3.exe was not found in the game folder.",
        bad_exe: "This sh3.exe is not the supported version.",
        missing_folder: "Part of the installation is missing:",
    },
    Text {
        title: "Préparer Silent Hill 3 pour la Vita",
        intro: "Copie vos propres fichiers du jeu, déchiffre les films et les convertit pour le décodeur vidéo de la Vita. Aucun fichier du jeu n'est envoyé nulle part.",
        language: "Langue de l'interface",
        game: "Dossier du jeu",
        game_hint: "Le dossier qui contient sh3.exe et le dossier data.",
        output: "Dossier de sortie",
        output_hint: "Un dossier séparé : un dossier sh3 y est créé, environ 5 Go.",
        browse: "Parcourir…",
        movies: "Convertir les films en MP4 (recommandé)",
        movies_hint: "Utilise le ffmpeg intégré à sh3prep (ou celui installé sur cet ordinateur). Sans lui, les films jouent à environ 20 images/s.",
        prepare: "Préparer les fichiers",
        ready: "Choisissez les deux dossiers, puis lancez la préparation.",
        checking: "Vérification des fichiers du jeu…",
        copying: "Préparation…",
        files: "fichiers",
        complete: "Préparation terminée.",
        transfer: "Copiez le dossier sh3 dans ux0:data/ sur la Vita (l'USB est bien plus rapide que le FTP) :",
        failed: "La préparation a échoué",
        no_ffmpeg: "ffmpeg est introuvable : les films n'ont pas été convertis en MP4 et joueront à environ 20 images/s. Installez ffmpeg et relancez la préparation.",
        footer: "sh3prep fait partie de sh3vita (GPL-3.0). Silent Hill est une marque de Konami ; ce projet n'est pas affilié à Konami.",
        missing_exe: "sh3.exe est introuvable dans le dossier du jeu.",
        bad_exe: "Ce sh3.exe n'est pas la version prise en charge.",
        missing_folder: "Une partie de l'installation manque :",
    },
];

impl Text {
    pub fn error(&self, e: &Error) -> String {
        match e {
            Error::MissingExe(_) => self.missing_exe.to_owned(),
            Error::UnsupportedExe(sum) => format!("{} (SHA-256 {sum})", self.bad_exe),
            Error::MissingFolder(name) => format!("{} {name}", self.missing_folder),
            Error::Io { path, source } => format!("{}: {source}", path.display()),
        }
    }
}
