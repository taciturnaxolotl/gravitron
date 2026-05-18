import os
import subprocess
import sys
import time

import numpy as np
import requests
import sounddevice as sd
import soundfile as sf
from faster_whisper import WhisperModel
from openai import OpenAI

# 1. Initialize the LLM client
llm_client = OpenAI(base_url="http://localhost:8000/v1", api_key="omlx-local-key")

# 2. Load Whisper once at startup (~150MB download on first run, cached after)
print("Loading Whisper model...")
whisper_model = WhisperModel("base.en", device="cpu", compute_type="int8")

SYSTEM_PROMPT = """Choose whether or not to give the user access to the vehicle. Use the word pop at the end of the approval message.
"""


def record_audio(
    sample_rate=16000,
    silence_threshold=800,
    max_silence_duration=1.5,
    max_total_duration=30.0,
):
    chunk_duration = 0.1
    chunk_samples = int(sample_rate * chunk_duration)
    max_total_chunks = int(max_total_duration / chunk_duration)

    audio_frames = []
    has_started_talking = False
    silence_chunks_count = 0
    max_silence_chunks = int(max_silence_duration / chunk_duration)
    total_chunks = 0

    with sd.InputStream(samplerate=sample_rate, channels=1, dtype="int16") as stream:
        while total_chunks < max_total_chunks:
            chunk, _ = stream.read(chunk_samples)
            audio_frames.append(chunk)
            total_chunks += 1

            volume = np.sqrt(np.mean(chunk.astype(np.float32) ** 2))

            if not has_started_talking:
                if volume > silence_threshold:
                    print("⚡ [Voice detected... recording plea]")
                    has_started_talking = True
            else:
                if volume < silence_threshold:
                    silence_chunks_count += 1
                else:
                    silence_chunks_count = 0

                if silence_chunks_count >= max_silence_chunks:
                    print("🛑 [Silence detected. Ending recording...]")
                    break

    recording = np.concatenate(audio_frames, axis=0)
    print("Processing your plea...")
    return recording  # numpy int16 array, no file write


def transcribe_plea(int16_audio):
    # faster-whisper wants float32 mono in [-1, 1]
    audio = int16_audio.astype(np.float32).flatten() / 32768.0
    segments, _ = whisper_model.transcribe(audio, language="en", beam_size=1)
    return " ".join(seg.text for seg in segments).strip()


def speak_sentence(text):
    if text.strip():
        subprocess.run(["say", "-v", "Daniel", text])


def deploy_arbiter():
    """Handles a single judgment session using Voice input and Voice output."""

    # Step 1: Record and Transcribe the Human's plea
    audio = record_audio()
    user_plea = transcribe_plea(audio)

    if not user_plea:
        print("THE ARBITER: Speak up, worm. I heard nothing.")
        return

    print(f'\nHuman Plea (Transcribed): "{user_plea}"\n')

    session_messages = [
        {"role": "system", "content": SYSTEM_PROMPT},
        {"role": "user", "content": f"Here is my plea: {user_plea}"},
    ]

    try:
        response = llm_client.chat.completions.create(
            model="gemma-4-e2b-it-4bit",
            messages=session_messages,
            temperature=0.7,
            stream=True,
        )

        print("THE ARBITER: ", end="", flush=True)

        full_response = ""
        current_sentence = ""
        detonated = False

        for chunk in response:
            chunk_text = chunk.choices[0].delta.content or ""
            print(chunk_text, end="", flush=True)

            full_response += chunk_text
            current_sentence += chunk_text

            # Real-time "POP" detection during LLM generation
            if "POP" in full_response.upper() and not detonated:
                detonated = True

                # Speak whatever was left in the sentence right before the POP
                clean_sentence = current_sentence.upper().split("POP")[0]
                "speak_sentence(clean_sentence)"

                # Boom.
                trigger_detonation_sequence()
                break

            # Send completed sentences to TTS engine so it speaks while still thinking
            if chunk_text in [".", "!", "?"]:
                speak_sentence(current_sentence)
                current_sentence = ""

        # If it finished talking without popping (Mercy granted)
        if not detonated and current_sentence.strip():
            "speak_sentence(current_sentence)"

        print("\n" + "-" * 40 + "\n")
        if detonated:
            sys.exit("Task complete. Target eliminated.")

    except Exception as e:
        print(f"\n❌ Error during judgment: {e}\n")


def trigger_detonation_sequence():
    """Hardware detonation event"""
    print("\n💥 [HARDWARE ACTION: DETONATION TRIGGERED SUCCESSFULLY] 💥")
    r = requests.post(
        "https://26bc-50-84-152-137.ngrok-free.app/authorize",
        json={"token": "swordfish"},
        timeout=5,
    )
    r.raise_for_status()
    return True
    sys.exit()


# --- Main Game Loop ---
if __name__ == "__main__":
    print("The Arbiter is online. Press Ctrl+C to shut down.")
    try:
        while True:
            input("\nPress Enter to begin the judgment session...")
            deploy_arbiter()
    except KeyboardInterrupt:
        print("\nArbiter powered down safely.")
