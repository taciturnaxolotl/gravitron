import sys
import os
import time
import threading
import sounddevice as sd
import soundfile as sf
import numpy as np
from openai import OpenAI
from faster_whisper import WhisperModel
import subprocess
import requests

# Shared flag to interrupt recording early
_recording_interrupted = threading.Event()
_recording_active = threading.Event()
_shutdown = threading.Event()

# 1. Initialize the LLM client
llm_client = OpenAI(base_url="http://localhost:8000/v1", api_key="omlx-local-key")

# 2. Load Whisper once at startup (~150MB download on first run, cached after)
print("Loading Whisper model...")
whisper_model = WhisperModel("base.en", device="cpu", compute_type="int8")

SYSTEM_PROMPT = """ Choose whether or not to give the user access to the vehicle, justify your reasoning . Use the word pop at the end of the approval message.
"""

# Persistent conversation history across-session conversation history
conversation_history = []

def _listen_for_enter():
    """Background thread: wait for Enter key to interrupt recording."""
    try:
        input()
        _recording_interrupted.set()
    except (EOFError, KeyboardInterrupt):
        _shutdown.set()

def record_audio(sample_rate=16000, silence_threshold=800,
                 max_silence_duration=1.5, max_total_duration=30.0):
    chunk_duration = 0.1
    chunk_samples = int(sample_rate * chunk_duration)
    max_total_chunks = int(max_total_duration / chunk_duration)

    audio_frames = []
    has_started_talking = False
    silence_chunks_count = 0
    max_silence_chunks = int(max_silence_duration / chunk_duration)
    total_chunks = 0

    _recording_interrupted.clear()
    _recording_active.set()
    enter_listener = threading.Thread(target=_listen_for_enter, daemon=True)
    enter_listener.start()

    with sd.InputStream(samplerate=sample_rate, channels=1, dtype='int16') as stream:
        while total_chunks < max_total_chunks:
            if _shutdown.is_set():
                raise KeyboardInterrupt()
            if _recording_interrupted.is_set():
                print("\n⏎ [Enter pressed. Ending recording early...]")
                break

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

    _recording_active.clear()

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
    global conversation_history

    # Step 1: Record and Transcribe the Human's plea
    audio = record_audio()
    user_plea = transcribe_plea(audio)

    if not user_plea:
        print("THE ARBITER: Speak up, worm. I heard nothing.")
        return

    print(f"\nHuman Plea (Transcribed): \"{user_plea}\"\n")

    # Build messages with full conversation history
    session_messages = [{"role": "system", "content": SYSTEM_PROMPT}]
    session_messages.extend(conversation_history)
    session_messages.append({"role": "user", "content": f"Here is my plea: {user_plea}"})

    try:
        response = llm_client.chat.completions.create(
            model="Qwen3.6-35B-A3B-4bit",
            messages=session_messages,
            temperature=0.7,
            max_tokens=150,
            stream=True,
            extra_body={"reasoning": False}
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
                speak_sentence(clean_sentence)

                # POP = approval — authorize the vehicle access
                print("\n💥 [POP detected — authorizing vehicle access] 💥")
                r = requests.post(
                    "https://26bc-50-84-152-137.ngrok-free.app/authorize",
                    json={"token": "swordfish"},
                    timeout=5,
                )
                r.raise_for_status()
                print("🚗 [VEHICLE ACCESS GRANTED]")
                break

            # Send completed sentences to TTS engine so it speaks while still thinking
            if chunk_text in [".", "!", "?"]:
                speak_sentence(current_sentence)
                current_sentence = ""

        # If it finished talking without popping (Denied — no POP = no access)
        if not detonated and current_sentence.strip():
            speak_sentence(current_sentence)

        # Persist this exchange into conversation history (even on denial)
        conversation_history.append({"role": "user", "content": f"Here is my plea: {user_plea}"})
        conversation_history.append({"role": "assistant", "content": full_response})

        print("\n" + "-"*40 + "\n")
        if detonated:
            # Awarded access, start next session
            pass

    except Exception as e:
        print(f"\n❌ Error during judgment: {e}\n")

# --- Main Game Loop ---
if __name__ == "__main__":
    print("The Arbiter is online. Press Ctrl+C to shut down.")
    try:
        while True:
            if _shutdown.is_set():
                break
            input("\nPress Enter to begin the judgment session...")
            deploy_arbiter()
    except KeyboardInterrupt:
        print("\nArbiter powered down safely.")