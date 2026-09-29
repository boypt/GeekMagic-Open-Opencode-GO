/**
 * opencode-notice-plugin — forward OpenCode v2 events to this firmware's
 * temporary notice overlay.
 *
 * Purpose
 * -------
 * The device (ESP8266 + ST7789 240x240) exposes `POST /api/v1/notice`, which
 * paints a full-screen ASCII notice for 1..30 seconds and then returns to the
 * previous scene. This plugin subscribes to the OpenCode event stream and pushes
 * the same kind of events the built-in desktop notifier reacts to, so a glance
 * at the desk screen tells you that a run finished, failed, was interrupted, is
 * blocked on a permission prompt, or is waiting on a form.
 *
 * Event coverage (mirrors the built-in notifier)
 * ----------------------------------------------
 *   session.execution.succeeded    -> info      "Done: <session title>"
 *   session.execution.interrupted  -> warning   "Stopped: <session title>"  (+ reason)
 *   session.execution.failed       -> critical  "Failed: <session title>"  (+ error)
 *   permission.asked               -> warning   "Permission needed"        (+ action/resources)
 *   form.created                   -> info      "Form: <form title>"
 *
 * Sessions with a `parentID` are subagent sessions; they are skipped so a swarm
 * of subagents cannot spam the screen.
 *
 * Device constraints honoured here (see ../AGENTS.md, notice section)
 * -------------------------------------------------------------------
 *   - `text` must be 1..199 bytes of printable ASCII (0x20..0x7E) plus '\n'.
 *     Anything else (CJK, tabs, control bytes) is a hard 400, so all event /
 *     session / error text is sanitized before it is sent.
 *   - `seconds` must be an integer 1..30 (anything else is a 400).
 *   - `level` is one of info | warning | critical.
 *   - Auth is a Bearer token; the body must stay under 1 KB.
 *
 * Options (opencode.jsonc) with environment fallback
 * --------------------------------------------------
 *   device    -> OPENCODE_NOTICE_DEVICE     e.g. "http://192.168.1.42" (no
 *                                                 trailing slash needed)
 *   token     -> OPENCODE_NOTICE_TOKEN      device Bearer token
 *   seconds   -> OPENCODE_NOTICE_SECONDS    1..30, default 5
 *   timeoutMs -> OPENCODE_NOTICE_TIMEOUT_MS request budget, default 4000
 *   enabled   -> OPENCODE_NOTICE_ENABLED    "0"/"false" disables the plugin
 *
 * There is no default device: with neither options nor environment set the
 * plugin loads, logs one warning, and stays inert. It never guesses localhost
 * and never sends an unauthenticated request.
 *
 * Local package plugin config example (project opencode.jsonc):
 *
 *   {
 *     "$schema": "https://opencode.ai/config.json",
 *     "plugins": [
 *       {
 *         "package": "./tools/opencode-notice-plugin.ts",
 *         "options": {
 *           "device": "http://192.168.1.42",
 *           "token": "<api_token from data/config.json>",
 *           "seconds": 5
 *         }
 *       }
 *     ]
 *   }
 *
 * Dependencies: none (uses the v2 plain-object plugin shape and global fetch).
 */

const LOG_PREFIX = "[notice-plugin]"

// Firmware limits, mirrored from src/web/Api.cpp (handleNoticeSet).
const MAX_TEXT_BYTES = 199
const MAX_BODY_BYTES = 1024
const MIN_SECONDS = 1
const MAX_SECONDS = 30
const DEFAULT_SECONDS = 5
const DEFAULT_TIMEOUT_MS = 4000
// GET /api/v1/notice is the documented way to confirm the token and reachability.
const SESSION_CACHE_TTL_MS = 30_000

type NoticeLevel = "info" | "warning" | "critical"

type Notice = {
  level: NoticeLevel
  text: string
}

type SessionInfo = {
  id?: string
  parentID?: string
  title?: string
}

/** Events we care about, narrowed to just the fields we read. */
type NoticeEvent = {
  type: string
  data?: {
    sessionID?: string
    reason?: string
    error?: { type?: string; message?: string; name?: string }
    action?: string
    resources?: string[]
    message?: string
    form?: { title?: string; sessionID?: string }
  }
}

function log(level: "info" | "warn" | "error", message: string, extra?: unknown) {
  const line = `${LOG_PREFIX} ${message}`
  if (extra === undefined) {
    if (level === "error") console.error(line)
    else if (level === "warn") console.warn(line)
    else console.log(line)
    return
  }
  if (level === "error") console.error(line, extra)
  else if (level === "warn") console.warn(line, extra)
  else console.log(line, extra)
}

function readEnv(name: string): string | undefined {
  const value = (globalThis as { process?: { env?: Record<string, string | undefined> } }).process?.env?.[name]
  if (typeof value !== "string") return undefined
  const trimmed = value.trim()
  return trimmed === "" ? undefined : trimmed
}

function readOption(options: Record<string, unknown> | undefined, key: string): unknown {
  if (!options) return undefined
  const value = options[key]
  return value === undefined || value === null || value === "" ? undefined : value
}

function resolveDevice(options: Record<string, unknown> | undefined): string | undefined {
  const raw = readOption(options, "device") ?? readEnv("OPENCODE_NOTICE_DEVICE")
  if (typeof raw !== "string") return undefined
  const trimmed = raw.trim()
  if (trimmed === "") return undefined
  // Accept "192.168.1.42" and "http://host:80/" as well as a full URL.
  const withScheme = /^https?:\/\//i.test(trimmed) ? trimmed : `http://${trimmed}`
  try {
    const url = new URL(withScheme)
    return url.origin
  } catch {
    return undefined
  }
}

function resolveToken(options: Record<string, unknown> | undefined): string | undefined {
  const raw = readOption(options, "token") ?? readEnv("OPENCODE_NOTICE_TOKEN")
  if (typeof raw !== "string") return undefined
  const trimmed = raw.trim()
  return trimmed === "" ? undefined : trimmed
}

function clampSeconds(value: unknown, fallback: number): number {
  const parsed = typeof value === "number" ? value : typeof value === "string" ? Number.parseInt(value, 10) : Number.NaN
  if (!Number.isFinite(parsed)) return fallback
  const rounded = Math.trunc(parsed)
  if (rounded < MIN_SECONDS) return MIN_SECONDS
  if (rounded > MAX_SECONDS) return MAX_SECONDS
  return rounded
}

function resolveTimeoutMs(options: Record<string, unknown> | undefined): number {
  const fallback = DEFAULT_TIMEOUT_MS
  const raw = readOption(options, "timeoutMs") ?? readEnv("OPENCODE_NOTICE_TIMEOUT_MS")
  const parsed = typeof raw === "number" ? raw : typeof raw === "string" ? Number.parseInt(raw, 10) : Number.NaN
  if (!Number.isFinite(parsed) || parsed <= 0) return fallback
  return Math.min(Math.trunc(parsed), 30_000)
}

function resolveEnabled(options: Record<string, unknown> | undefined): boolean {
  const raw = readOption(options, "enabled") ?? readEnv("OPENCODE_NOTICE_ENABLED")
  if (raw === undefined) return true
  const text = String(raw).trim().toLowerCase()
  return !(text === "0" || text === "false" || text === "no" || text === "off")
}

/**
 * Reduce arbitrary text to the exact byte set the firmware accepts:
 * printable ASCII plus '\n'. CJK / emoji / tabs / control bytes are dropped
 * (a run of them collapses to nothing rather than a wall of '?'), and the
 * result is trimmed, whitespace-collapsed and cut to 199 bytes.
 */
function sanitizeText(input: unknown, fallback: string): string {
  if (typeof input !== "string" || input === "") return fallback

  let out = ""
  let pendingNewlines = 0
  for (const char of input) {
    if (char === "\r") continue
    if (char === "\n") {
      pendingNewlines = out === "" ? 0 : 1
      continue
    }
    const code = char.codePointAt(0) ?? 0
    if (code === 0x09 || code === 0x0b || code === 0x0c) {
      // Whitespace-ish controls the firmware rejects: treat as a space.
      if (out !== "" && !out.endsWith(" ")) out += " "
      continue
    }
    if (code < 0x20 || code > 0x7e) continue
    if (pendingNewlines > 0) {
      out += "\n"
      pendingNewlines = 0
    }
    if (out === "" || out.endsWith(" ")) {
      if (char === " ") continue
    }
    out += char
    if (out.length >= MAX_TEXT_BYTES) break
  }

  out = out.replace(/[ ]+/g, " ").replace(/ *\n */g, "\n").replace(/\n{2,}/g, "\n").trim()
  if (out.length > MAX_TEXT_BYTES) {
    out = `${out.slice(0, MAX_TEXT_BYTES - 3).trimEnd()}...`
  }
  return out === "" ? fallback : out
}

function joinLines(...lines: (string | undefined)[]): string {
  return lines.filter((line): line is string => typeof line === "string" && line !== "").join("\n")
}

function firstLine(text: string, max = 120): string {
  const line = text.split("\n").find((candidate) => candidate.trim() !== "") ?? ""
  return line.length > max ? line.slice(0, max).trimEnd() : line
}

export default {
  id: "geekmagic-notice",

  async setup(ctx: any) {
    const options = ctx.options as Record<string, unknown> | undefined
    if (!resolveEnabled(options)) {
      log("info", "disabled by options/env, no notices will be sent")
      return
    }

    const device = resolveDevice(options)
    const token = resolveToken(options)
    const seconds = clampSeconds(readOption(options, "seconds") ?? readEnv("OPENCODE_NOTICE_SECONDS"), DEFAULT_SECONDS)
    const timeoutMs = resolveTimeoutMs(options)

    if (!device || !token) {
      // No default endpoint on purpose: posting to localhost with a missing
      // token would just produce a guaranteed 401 and hide the misconfiguration.
      log("warn", `inert: set device+token via plugin options or OPENCODE_NOTICE_DEVICE/OPENCODE_NOTICE_TOKEN (device=${device ?? "unset"}, token=${token ? "set" : "unset"})`)
      return
    }

    const endpoint = `${device}/api/v1/notice`
    log("info", `forwarding to ${endpoint} (seconds=${seconds}, timeout=${timeoutMs}ms)`)

    const controller = new AbortController()
    const sessionCache = new Map<string, { at: number; subagent: boolean; title: string }>()

    async function describeSession(sessionID: string | undefined) {
      if (!sessionID) return { subagent: false, title: "" }

      const cached = sessionCache.get(sessionID)
      if (cached && Date.now() - cached.at < SESSION_CACHE_TTL_MS) return cached

      let entry = { subagent: false, title: "" }
      try {
        const info = (await ctx.session.get({ sessionID })) as SessionInfo | undefined
        if (info) {
          entry = {
            subagent: typeof info.parentID === "string" && info.parentID !== "",
            title: typeof info.title === "string" ? info.title : "",
          }
        }
      } catch (error) {
        // A missing/renamed session must not stop the notification.
        log("warn", `session lookup failed for ${sessionID}: ${describeError(error)}`)
      }

      sessionCache.set(sessionID, { ...entry, at: Date.now() })
      if (sessionCache.size > 64) {
        const oldest = sessionCache.keys().next()
        if (!oldest.done) sessionCache.delete(oldest.value)
      }
      return entry
    }

    function describeError(error: unknown): string {
      if (error instanceof Error) return error.message
      if (typeof error === "string") return error
      if (error && typeof error === "object") {
        const message = (error as { message?: unknown }).message
        if (typeof message === "string") return message
      }
      return "unknown error"
    }

    async function post(notice: Notice) {
      const text = sanitizeText(notice.text, "")
      if (text === "") return

      const body = JSON.stringify({ level: notice.level, text, seconds })
      if (body.length >= MAX_BODY_BYTES) {
        log("warn", `dropping oversized notice (${body.length} bytes)`)
        return
      }

      const timeout = AbortSignal.timeout(timeoutMs)
      try {
        const response = await fetch(endpoint, {
          method: "POST",
          headers: {
            "Content-Type": "application/json",
            Authorization: `Bearer ${token}`,
          },
          body,
          signal: AbortSignal.any([controller.signal, timeout]),
        })
        if (!response.ok) {
          const detail = await response.text().catch(() => "")
          log("warn", `device returned ${response.status} ${detail.slice(0, 200)}`)
        }
      } catch (error) {
        // Timeouts, refused connections, aborts: log and move on, never throw
        // out of the event handler.
        log("warn", `notice post failed: ${describeError(error)}`)
      }
    }

    /** Fire-and-forget so event delivery is never blocked on the device HTTP call. */
    function notify(notice: Notice | undefined) {
      if (!notice) return
      void post(notice).catch((error) => log("warn", `unhandled notice error: ${describeError(error)}`))
    }

    async function buildNotice(event: NoticeEvent): Promise<Notice | undefined> {
      const data = event.data ?? {}
      switch (event.type) {
        case "session.execution.succeeded": {
          const session = await describeSession(data.sessionID)
          if (session.subagent) return undefined
          const title = sanitizeText(session.title, "").replace(/\n/g, " ")
          return { level: "info", text: sanitizeText(joinLines("Done", title || sessionLabel(data.sessionID)), "Done") }
        }
        case "session.execution.interrupted": {
          const session = await describeSession(data.sessionID)
          if (session.subagent) return undefined
          const reason = sanitizeText(data.reason, "").replace(/\n/g, " ")
          const title = sanitizeText(session.title, "").replace(/\n/g, " ")
          return {
            level: "warning",
            text: sanitizeText(joinLines("Stopped", title || sessionLabel(data.sessionID), reason && `reason: ${reason}`), "Stopped"),
          }
        }
        case "session.execution.failed": {
          const session = await describeSession(data.sessionID)
          if (session.subagent) return undefined
          const title = sanitizeText(session.title, "").replace(/\n/g, " ")
          const error = data.error
          const detail = sanitizeText(joinLines(error?.name ?? "", error?.message ?? data.message ?? ""), "")
          return {
            level: "critical",
            text: sanitizeText(joinLines("Failed", title || sessionLabel(data.sessionID), firstLine(detail)), "Failed"),
          }
        }
        case "permission.asked": {
          const session = await describeSession(data.sessionID)
          if (session.subagent) return undefined
          const action = sanitizeText(data.action ?? "", "").replace(/\n/g, " ")
          const resources = (data.resources ?? [])
            .map((resource) => sanitizeText(resource, "").replace(/\n/g, " "))
            .filter((resource) => resource !== "")
            .slice(0, 2)
          const message = firstLine(sanitizeText(data.message ?? "", ""))
          return {
            level: "warning",
            text: sanitizeText(
              joinLines("Permission needed", titleOrId(session, data.sessionID), action, message, resources.map((resource) => `- ${resource}`).join("\n")),
              "Permission needed",
            ),
          }
        }
        case "form.created": {
          const form = data.form
          const session = await describeSession(form?.sessionID ?? data.sessionID)
          if (session.subagent) return undefined
          const formTitle = sanitizeText(form?.title ?? "", "").replace(/\n/g, " ")
          return {
            level: "info",
            text: sanitizeText(joinLines("Form", formTitle || titleOrId(session, form?.sessionID ?? data.sessionID)), "Form"),
          }
        }
        default:
          return undefined
      }
    }

    function sessionLabel(sessionID: string | undefined): string {
      if (!sessionID) return ""
      // Long opaque ids are noise on a 240px screen; keep a short tail.
      return sessionID.length > 12 ? `session ${sessionID.slice(-6)}` : sessionID
    }

    function titleOrId(session: { title: string }, sessionID: string | undefined): string {
      const title = sanitizeText(session.title, "").replace(/\n/g, " ")
      return title || sessionLabel(sessionID)
    }

    // The event stream is long-lived: awaiting it inline would make setup()
    // itself never resolve, so the host would never receive the cleanup
    // function and could never abort the subscription. Consume it in a
    // detached task instead and hand the cleanup back synchronously.
    void (async () => {
      try {
        for await (const raw of ctx.event.subscribe({ signal: controller.signal })) {
          try {
            const event = raw as NoticeEvent
            notify(await buildNotice(event))
          } catch (error) {
            // One malformed event must not tear down the subscription.
            log("warn", `event handling failed: ${describeError(error)}`)
          }
        }
      } catch (error) {
        if (!controller.signal.aborted) {
          log("error", `event subscription stopped: ${describeError(error)}`)
        }
      }
    })()

    return () => {
      controller.abort()
    }
  },
}
