// embg Voice Assistant — frontend logic.
//
// Flow per user message:
//   1. POST /api/chat {text}
//   2. Render pipeline stages as chips: ASR (transcript + confidence),
//      NLU (intent + confidence), TTS (ready)
//   3. Render the assistant reply bubble
//   4. Speak the reply via the Web Speech API (speechSynthesis) — this is
//      the actual audio TTS output; the server produces the speakable text.

const chatEl = document.getElementById("chat");
const formEl = document.getElementById("composer");
const inputEl = document.getElementById("input");
const sendBtn = document.getElementById("sendBtn");
const ttsToggle = document.getElementById("ttsToggle");

let ttsEnabled = true;

// ── Helpers ──────────────────────────────────────────────────────────────────

function el(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

function pct(x) {
  return Math.round((x ?? 0) * 100) + "%";
}

function scrollToEnd() {
  chatEl.scrollTop = chatEl.scrollHeight;
}

// ── TTS (Web Speech API) ─────────────────────────────────────────────────────

function speak(text) {
  if (!ttsEnabled || !("speechSynthesis" in window)) return;
  window.speechSynthesis.cancel();
  const utterance = new SpeechSynthesisUtterance(text);
  utterance.rate = 1.02;
  utterance.pitch = 1.0;
  window.speechSynthesis.speak(utterance);
}

ttsToggle.addEventListener("click", () => {
  ttsEnabled = !ttsEnabled;
  ttsToggle.classList.toggle("off", !ttsEnabled);
  ttsToggle.setAttribute("aria-pressed", String(ttsEnabled));
  ttsToggle.textContent = ttsEnabled ? "🔊 TTS on" : "🔇 TTS off";
  if (!ttsEnabled && "speechSynthesis" in window) {
    window.speechSynthesis.cancel();
  }
});

// ── Rendering ────────────────────────────────────────────────────────────────

function chip(cls, label, value) {
  const c = el("span", `chip ${cls}`);
  c.appendChild(el("span", "dot"));
  c.appendChild(el("span", null, label));
  c.appendChild(el("b", null, value));
  return c;
}

function renderTurn(userText) {
  const turn = el("div", "turn");

  // User bubble — what the user typed (and what ASR "heard").
  const userBubble = el("div", "bubble user", userText);
  turn.appendChild(userBubble);

  // Stage row placeholder (filled when the API responds).
  const stageRow = el("div", "stage-row");
  turn.appendChild(stageRow);

  chatEl.appendChild(turn);
  scrollToEnd();
  return { turn, stageRow };
}

function renderStages(stageRow, data) {
  stageRow.replaceChildren(
    chip("asr", "ASR", data.asr.transcript || "—"),
    chip("nlu", "NLU", `${data.nlu.intent} · ${pct(data.nlu.confidence)}`),
    chip("tts", "TTS", data.tts.ready ? "ready" : "skipped")
  );
}

function renderReply(turn, replyText) {
  const botBubble = el("div", "bubble bot", replyText);
  turn.appendChild(botBubble);

  // Replay button for the spoken output.
  if ("speechSynthesis" in window) {
    const speaker = el("button", "speaker", "🔈 replay");
    speaker.title = "Speak this reply again";
    speaker.addEventListener("click", () => speak(replyText));
    turn.appendChild(speaker);
  }
  scrollToEnd();
}

// ── API call ─────────────────────────────────────────────────────────────────

async function sendMessage(text) {
  const trimmed = text.trim();
  if (!trimmed) return;

  inputEl.value = "";
  inputEl.focus();

  const { turn, stageRow } = renderTurn(trimmed);

  const typing = el("div", "typing", "assistant is thinking…");
  chatEl.appendChild(typing);
  scrollToEnd();

  try {
    const res = await fetch("/api/chat", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ text: trimmed }),
    });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const data = await res.json();

    typing.remove();
    renderStages(stageRow, data);
    renderReply(turn, data.reply);
    speak(data.tts.text || data.reply);
  } catch (err) {
    typing.remove();
    renderStages(stageRow, {
      asr: { transcript: trimmed },
      nlu: { intent: "error", confidence: 0 },
      tts: { ready: false },
    });
    renderReply(turn, `Sorry — request failed (${err.message}). Is the server running?`);
  }
}

// ── Events ───────────────────────────────────────────────────────────────────

formEl.addEventListener("submit", (e) => {
  e.preventDefault();
  sendMessage(inputEl.value);
});

document.querySelectorAll(".hint").forEach((btn) => {
  btn.addEventListener("click", () => sendMessage(btn.dataset.text));
});
