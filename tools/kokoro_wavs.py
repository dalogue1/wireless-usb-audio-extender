#!/usr/bin/env python3
"""Generates the announcements with Kokoro TTS (Apache-2.0) via kokoro-onnx.

    pip install kokoro-onnx soundfile "misaki[ja,zh]"     (misaki: Japanese and Mandarin only)
    model files from github.com/thewh1teagle/kokoro-onnx/releases (model-files-v1.0):
    kokoro-v1.0.onnx (or kokoro-v1.0.int8.onnx) and voices-v1.0.bin, in the current folder

    python3 tools/kokoro_wavs.py               -> every pack below, into voice-packs/<pack>/
    python3 tools/kokoro_wavs.py es fr         -> just those
    python3 tools/kokoro_wavs.py en-us         -> voice-packs/en-us/ (the built-in voice is
                                                  voice/: copy there and run make_voice.py)

To add a language: add a pack below (voice, Kokoro language code, the 18 phrases).
The file names must stay as they are; they are what the local board looks for.
Each clip is saved like the built-in voice (tools/make_voice.py): 16 kHz mono 16-bit,
silence trimmed, peak -1 dBFS. A whole pack then fits the local board's ~1.4 MB drive.
"""
import os
import struct
import sys
import wave

from kokoro_onnx import Kokoro

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_voice import RATE, resample, trim_normalise  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

NAMES = ["mic_1", "mic_2", "mic_3", "mic_4", "mic_5", "mic_6", "mic_7", "mic_8", "mic_9",
         "link_up", "link_lost", "adapter_connected", "adapter_removed", "adapter_unsupported",
         "filter_on", "filter_strong", "filter_off", "no_signal"]

EN = ["Microphone one.", "Microphone two.", "Microphone three.", "Microphone four.", "Microphone five.",
      "Microphone six.", "Microphone seven.", "Microphone eight.", "Microphone nine.",
      "Microphone connected.", "Microphone link lost.",
      "Adapter connected.", "Adapter removed.", "Adapter not supported.",
      "Noise filter on.", "Noise filter strong.", "Noise filter off.",
      "No microphone signal."]

# pack: (language name, Kokoro voice, phonemizer language, g2p, phrases in NAMES order)
# g2p None = Kokoro's own (espeak-ng); "ja" / "zh" = misaki.
PACKS = {
    "en-us": ("English (American)", "af_heart", "en-us", None, EN),
    "en-gb": ("English (British)", "bf_emma", "en-gb", None, EN),
    "es": ("Spanish", "ef_dora", "es", None, [
        "Micrófono uno.", "Micrófono dos.", "Micrófono tres.", "Micrófono cuatro.", "Micrófono cinco.",
        "Micrófono seis.", "Micrófono siete.", "Micrófono ocho.", "Micrófono nueve.",
        "Micrófono conectado.", "Enlace del micrófono perdido.",
        "Adaptador conectado.", "Adaptador desconectado.", "Adaptador no compatible.",
        "Filtro de ruido activado.", "Filtro de ruido fuerte.", "Filtro de ruido desactivado.",
        "Sin señal de micrófono."]),
    "fr": ("French", "ff_siwis", "fr-fr", None, [
        "Microphone un.", "Microphone deux.", "Microphone trois.", "Microphone quatre.", "Microphone cinq.",
        "Microphone six.", "Microphone sept.", "Microphone huit.", "Microphone neuf.",
        "Microphone connecté.", "Liaison du microphone perdue.",
        "Adaptateur connecté.", "Adaptateur retiré.", "Adaptateur non pris en charge.",
        "Filtre anti-bruit activé.", "Filtre anti-bruit renforcé.", "Filtre anti-bruit désactivé.",
        "Aucun signal du microphone."]),
    "it": ("Italian", "if_sara", "it", None, [
        "Microfono uno.", "Microfono due.", "Microfono tre.", "Microfono quattro.", "Microfono cinque.",
        "Microfono sei.", "Microfono sette.", "Microfono otto.", "Microfono nove.",
        "Microfono collegato.", "Collegamento del microfono perso.",
        "Adattatore collegato.", "Adattatore rimosso.", "Adattatore non supportato.",
        "Filtro antirumore attivo.", "Filtro antirumore forte.", "Filtro antirumore disattivato.",
        "Nessun segnale dal microfono."]),
    "pt-br": ("Portuguese (Brazilian)", "pf_dora", "pt-br", None, [
        "Microfone um.", "Microfone dois.", "Microfone três.", "Microfone quatro.", "Microfone cinco.",
        "Microfone seis.", "Microfone sete.", "Microfone oito.", "Microfone nove.",
        "Microfone conectado.", "Conexão do microfone perdida.",
        "Adaptador conectado.", "Adaptador removido.", "Adaptador não compatível.",
        "Filtro de ruído ligado.", "Filtro de ruído forte.", "Filtro de ruído desligado.",
        "Sem sinal do microfone."]),
    "hi": ("Hindi", "hf_alpha", "hi", None, [
        "माइक्रोफ़ोन एक।", "माइक्रोफ़ोन दो।", "माइक्रोफ़ोन तीन।", "माइक्रोफ़ोन चार।", "माइक्रोफ़ोन पाँच।",
        "माइक्रोफ़ोन छह।", "माइक्रोफ़ोन सात।", "माइक्रोफ़ोन आठ।", "माइक्रोफ़ोन नौ।",
        "माइक्रोफ़ोन जुड़ गया।", "माइक्रोफ़ोन का संपर्क टूट गया।",
        "एडैप्टर जुड़ गया।", "एडैप्टर हटा दिया गया।", "यह एडैप्टर समर्थित नहीं है।",
        "शोर फ़िल्टर चालू।", "शोर फ़िल्टर मज़बूत।", "शोर फ़िल्टर बंद।",
        "माइक्रोफ़ोन से कोई सिग्नल नहीं।"]),
    "ja": ("Japanese", "jf_alpha", "ja", "ja", [
        "マイク、いち。", "マイク、に。", "マイク、さん。", "マイク、よん。", "マイク、ご。",
        "マイク、ろく。", "マイク、なな。", "マイク、はち。", "マイク、きゅう。",
        "マイクが接続されました。", "マイクの接続が切れました。",
        "アダプターが接続されました。", "アダプターが取り外されました。", "このアダプターには対応していません。",
        "ノイズフィルター、オン。", "ノイズフィルター、強め。", "ノイズフィルター、オフ。",
        "マイクの信号がありません。"]),
    "zh": ("Mandarin Chinese", "zf_xiaobei", "cmn", "zh", [
        "一号麦克风。", "二号麦克风。", "三号麦克风。", "四号麦克风。", "五号麦克风。",
        "六号麦克风。", "七号麦克风。", "八号麦克风。", "九号麦克风。",
        "麦克风已连接。", "麦克风连接已断开。",
        "适配器已连接。", "适配器已移除。", "不支持此适配器。",
        "降噪已开启。", "强力降噪已开启。", "降噪已关闭。",
        "没有麦克风信号。"]),
}


def g2p_for(kind):
    if kind == "ja":
        from misaki import ja
        g = ja.JAG2P()
    elif kind == "zh":
        from misaki import zh
        g = zh.ZHG2P()
    else:
        return None
    def phonemes(text):
        r = g(text)  # a string, or (phonemes, tokens) in some misaki versions
        return r if isinstance(r, str) else r[0]
    return phonemes


def save(path, samples, rate):
    y = trim_normalise(resample([float(v) * 32767 for v in samples], rate, RATE))
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(struct.pack("<%dh" % len(y), *y))


def main():
    model = "kokoro-v1.0.onnx" if os.path.exists("kokoro-v1.0.onnx") else "kokoro-v1.0.int8.onnx"
    k = Kokoro(model, "voices-v1.0.bin")
    for pack in sys.argv[1:] or [p for p in PACKS if p != "en-us"]:
        language, voice, lang, g2p_kind, phrases = PACKS[pack]
        assert len(phrases) == len(NAMES), pack
        g2p = g2p_for(g2p_kind)
        out = os.path.join(ROOT, "voice-packs", pack)
        os.makedirs(out, exist_ok=True)
        for name, text in zip(NAMES, phrases):
            if g2p:
                samples, rate = k.create(g2p(text), voice=voice, speed=1.0, lang=lang, is_phonemes=True)
            else:
                samples, rate = k.create(text, voice=voice, speed=1.0, lang=lang)
            save(os.path.join(out, name + ".wav"), samples, rate)
        with open(os.path.join(out, "phrases.txt"), "w", encoding="utf-8") as f:
            f.write("%s - Kokoro TTS voice %s (%s)\n\n" % (language, voice, model))
            for name, text, en in zip(NAMES, phrases, EN):
                f.write("%-20s %s%s\n" % (name + ".wav", text, "" if text == en else "   (" + en + ")"))
        print(pack, voice, "done")


if __name__ == "__main__":
    main()
