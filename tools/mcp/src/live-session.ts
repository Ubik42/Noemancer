import { opendir, readFile, realpath, stat } from "node:fs/promises";
import { tmpdir } from "node:os";
import { connect as connectSocket, type Socket } from "node:net";
import { isAbsolute, join, relative, resolve, win32 } from "node:path";

const descriptorSchema = "noemancer.live-editor-session/0.1";
const transportProtocol = "noemancer.live-editor/0.1";
const defaultDescriptorBytes = 64 * 1024;
const defaultCredentialBytes = 4 * 1024;
const defaultFrameBytes = 1024 * 1024;
const defaultTimeoutMs = 5_000;
const maxSessionIdBytes = 96;
const maxEndpointBytes = 512;
const maxCredentialFileBytes = 128;
const maxMethodBytes = 128;
const maxSessionScanEntries = 128;
const maxHealthProbes = 32;
const maxDiscoveryProbeMs = 500;

type JsonObject = Record<string, unknown>;

export type LiveSessionDescriptor = {
  readonly schemaVersion: string;
  readonly sessionId: string;
  readonly endpoint: string;
  readonly credentialFile: string;
  readonly [key: string]: unknown;
};

export type LiveSessionSelection = {
  readonly descriptorPath: string;
  readonly sessionsRoot: string;
  readonly descriptor: LiveSessionDescriptor;
  readonly credentialPath: string;
};

export type LiveSessionOptions = {
  /** Explicit session id or descriptor path. Defaults to NOEMANCER_EDITOR_SESSION. */
  readonly selection?: string;
  /** Defaults to NOEMANCER_EDITOR_SESSIONS_DIR or the local Noemancer session directory. */
  readonly sessionsRoot?: string;
  readonly timeoutMs?: number;
  readonly maxDescriptorBytes?: number;
  readonly maxCredentialBytes?: number;
  readonly maxFrameBytes?: number;
};

export type LiveSessionInvokeOptions = {
  readonly timeoutMs?: number;
};

type PendingRequest = {
  readonly resolve: (value: unknown) => void;
  readonly reject: (reason: Error) => void;
  readonly timer: NodeJS.Timeout;
};

type HelloWaiter = {
  readonly resolve: (message: JsonObject) => void;
  readonly reject: (reason: Error) => void;
};

function isObject(value: unknown): value is JsonObject {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function stringField(object: JsonObject, key: string): string | undefined {
  const value = object[key];
  return typeof value === "string" && value.length > 0 ? value : undefined;
}

function safeComponent(value: string, maximumBytes: number): boolean {
  return value.length > 0 && value.length <= maximumBytes &&
    /^[A-Za-z0-9][A-Za-z0-9._-]*$/u.test(value);
}

function safeSessionId(value: string): boolean {
  return safeComponent(value, maxSessionIdBytes);
}

function inside(root: string, candidate: string): boolean {
  const child = relative(resolve(root), resolve(candidate)).replaceAll("\\", "/");
  return child.length === 0 ||
    (child !== ".." && !child.startsWith("../") && !isAbsolute(child) && !win32.isAbsolute(child));
}

function relativeCredentialName(value: string): boolean {
  // The native session store publishes a root-relative filename, not a
  // path. Reject separators and traversal on both Windows and POSIX.
  return safeComponent(value, maxCredentialFileBytes);
}

function endpointFromDescriptor(object: JsonObject): string | undefined {
  const endpoint = stringField(object, "endpoint");
  if (endpoint !== undefined) return endpoint;
  for (const key of ["transport", "connection"]) {
    const nested = object[key];
    if (!isObject(nested)) continue;
    const nestedEndpoint = stringField(nested, "endpoint");
    if (nestedEndpoint !== undefined) return nestedEndpoint;
  }
  return undefined;
}

function defaultSessionsRoot(): string {
  const configured = process.env.NOEMANCER_EDITOR_SESSIONS_DIR?.trim();
  if (configured !== undefined && configured.length > 0) return resolve(configured);
  if (process.platform === "win32") {
    const localAppData = process.env.LOCALAPPDATA?.trim();
    if (localAppData !== undefined && localAppData.length > 0) {
      return join(localAppData, "Noemancer", "Sessions");
    }
  }
  const stateHome = process.env.XDG_STATE_HOME?.trim();
  if (stateHome !== undefined && stateHome.length > 0) {
    return join(stateHome, "Noemancer", "Sessions");
  }
  return join(tmpdir(), "Noemancer", "Sessions");
}

async function boundedRead(path: string, maximumBytes: number, label: string): Promise<string> {
  if (!Number.isSafeInteger(maximumBytes) || maximumBytes <= 0) {
    throw new Error(label + " size limit is invalid.");
  }
  let size: number;
  try {
    size = (await stat(path)).size;
  } catch {
    throw new Error(label + " could not be read.");
  }
  if (size > maximumBytes) throw new Error(label + " exceeds its bounded size limit.");
  let bytes: Buffer;
  try {
    bytes = await readFile(path);
  } catch {
    throw new Error(label + " could not be read.");
  }
  if (bytes.byteLength > maximumBytes) throw new Error(label + " exceeds its bounded size limit.");
  return bytes.toString("utf8");
}

async function existingRealPath(path: string, label: string): Promise<string> {
  try {
    return await realpath(path);
  } catch {
    throw new Error(label + " does not exist or is not accessible.");
  }
}

function parseDescriptor(text: string): LiveSessionDescriptor {
  let raw: unknown;
  try {
    raw = JSON.parse(text);
  } catch {
    throw new Error("Session descriptor is not valid JSON.");
  }
  if (!isObject(raw)) throw new Error("Session descriptor must be a JSON object.");

  for (const key of ["token", "authToken", "accessToken", "secret"]) {
    if (Object.prototype.hasOwnProperty.call(raw, key)) {
      throw new Error("Bearer credentials must remain in the referenced sidecar.");
    }
  }

  const schemaVersion = stringField(raw, "schemaVersion");
  const sessionId = stringField(raw, "sessionId");
  const endpoint = endpointFromDescriptor(raw);
  const credentialFile = stringField(raw, "credentialFile");
  if (schemaVersion !== descriptorSchema) {
    throw new Error("Session descriptor schemaVersion is unsupported.");
  }
  if (sessionId === undefined || !safeSessionId(sessionId)) {
    throw new Error("Session descriptor has an invalid sessionId.");
  }
  if (endpoint === undefined || endpoint.length > maxEndpointBytes ||
      /[\u0000-\u001f\u007f]/u.test(endpoint)) {
    throw new Error("Session descriptor has no bounded pipe endpoint.");
  }
  if (credentialFile === undefined || !relativeCredentialName(credentialFile)) {
    throw new Error("Session credentialFile must be a root-relative safe filename.");
  }

  // Keep useful non-secret discovery metadata while dropping unknown fields.
  // In particular, a malformed descriptor cannot smuggle a bearer token into
  // the public selection result.
  const descriptor: Record<string, unknown> = {
    schemaVersion,
    sessionId,
    endpoint,
    credentialFile,
  };
  for (const key of [
    "version",
    "processId",
    "processIdentity",
    "projectId",
    "projectName",
    "projectRoot",
    "capabilities",
    "createdUnixMilliseconds",
    "heartbeatUnixMilliseconds",
    "revision",
  ]) {
    if (Object.prototype.hasOwnProperty.call(raw, key)) descriptor[key] = raw[key];
  }
  return descriptor as LiveSessionDescriptor;
}

async function isDescriptorCandidate(path: string, maximumBytes: number): Promise<boolean> {
  try {
    const descriptor = parseDescriptor(await boundedRead(path, maximumBytes, "session descriptor"));
    return safeSessionId(descriptor.sessionId);
  } catch {
    return false;
  }
}

async function descriptorCandidates(root: string, maximumBytes: number): Promise<string[]> {
  let directory;
  try {
    directory = await opendir(root);
  } catch {
    return [];
  }
  const candidates = new Set<string>();
  let scannedEntries = 0;
  for await (const entry of directory) {
    scannedEntries += 1;
    if (scannedEntries > maxSessionScanEntries) {
      throw new Error("The editor sessions directory exceeds its bounded discovery limit.");
    }
    if (!entry.isFile() || !entry.name.toLowerCase().endsWith(".json")) continue;
    const stem = entry.name.slice(0, -5);
    if (!safeSessionId(stem)) continue;
    const candidate = join(root, entry.name);
    try {
      const descriptor = await existingRealPath(candidate, "session descriptor");
      if (!inside(root, descriptor) || !await isDescriptorCandidate(descriptor, maximumBytes)) continue;
      const parsed = parseDescriptor(await boundedRead(descriptor, maximumBytes, "session descriptor"));
      if (parsed.sessionId === stem) candidates.add(descriptor);
    } catch {
      // Discovery skips malformed entries. Explicit selection reports the
      // precise contract failure instead of silently connecting elsewhere.
    }
  }
  return [...candidates].sort((left, right) => left.localeCompare(right));
}

function looksLikePath(selection: string): boolean {
  return isAbsolute(selection) || win32.isAbsolute(selection) ||
    selection.includes("/") || selection.includes("\\") ||
    selection.toLowerCase().endsWith(".json");
}

async function selectedDescriptorPath(
  root: string,
  selection: string | undefined,
  maximumBytes: number,
): Promise<string> {
  if (selection !== undefined && selection.length > 0) {
    if (looksLikePath(selection)) {
      const descriptor = await existingRealPath(resolve(selection), "selected session descriptor");
      if (!inside(root, descriptor)) {
        throw new Error("Selected session descriptor is outside the sessions directory.");
      }
      return descriptor;
    }
    if (!safeSessionId(selection)) {
      throw new Error("NOEMANCER_EDITOR_SESSION must be a safe session id or descriptor path.");
    }
    const candidate = join(root, selection + ".json");
    let descriptor: string;
    try {
      descriptor = await existingRealPath(candidate, "selected session descriptor");
    } catch {
      throw new Error("The selected editor session was not found.");
    }
    if (!inside(root, descriptor)) {
      throw new Error("Selected session descriptor is outside the sessions directory.");
    }
    return descriptor;
  }
  const candidates = await descriptorCandidates(root, maximumBytes);
  if (candidates.length === 0) {
    throw new Error("No editor session descriptor was found; choose one explicitly.");
  }
  if (candidates.length !== 1) {
    throw new Error("Multiple editor sessions are available; set NOEMANCER_EDITOR_SESSION explicitly.");
  }
  return candidates[0]!;
}

async function selectionFromDescriptorPath(
  sessionsRoot: string,
  descriptorPath: string,
  options: LiveSessionOptions,
): Promise<LiveSessionSelection> {
  if (!inside(sessionsRoot, descriptorPath)) {
    throw new Error("Selected session descriptor is outside the sessions directory.");
  }
  const descriptor = parseDescriptor(await boundedRead(
    descriptorPath,
    options.maxDescriptorBytes ?? defaultDescriptorBytes,
    "session descriptor",
  ));

  const credentialPath = join(sessionsRoot, descriptor.credentialFile);
  if (!inside(sessionsRoot, credentialPath)) {
    throw new Error("Session credential is outside the sessions directory.");
  }
  const credentialRealPath = await existingRealPath(credentialPath, "session credential");
  if (!inside(sessionsRoot, credentialRealPath)) {
    throw new Error("Session credential resolves outside the sessions directory.");
  }
  return { descriptorPath, sessionsRoot, descriptor, credentialPath: credentialRealPath };
}

export async function resolveLiveSession(options: LiveSessionOptions = {}): Promise<LiveSessionSelection> {
  const rootPath = resolve(options.sessionsRoot ?? defaultSessionsRoot());
  const sessionsRoot = await existingRealPath(rootPath, "sessions directory");
  const selection = options.selection?.trim() ||
    process.env.NOEMANCER_EDITOR_SESSION?.trim() || undefined;
  const descriptorPath = await selectedDescriptorPath(
    sessionsRoot,
    selection,
    options.maxDescriptorBytes ?? defaultDescriptorBytes,
  );
  const result = await selectionFromDescriptorPath(sessionsRoot, descriptorPath, options);
  if (selection !== undefined && !looksLikePath(selection) &&
      result.descriptor.sessionId !== selection) {
    throw new Error("Selected session descriptor does not match the requested session id.");
  }
  return result;
}

/**
 * Returns every bounded, root-contained session descriptor whose credential
 * sidecar is present. This deliberately does not connect: callers that need
 * a live Editor must probe each selection and then reject zero/multiple
 * healthy sessions explicitly.
 */
export async function discoverLiveSessions(
  options: LiveSessionOptions = {},
): Promise<LiveSessionSelection[]> {
  const rootPath = resolve(options.sessionsRoot ?? defaultSessionsRoot());
  let sessionsRoot: string;
  try {
    sessionsRoot = await existingRealPath(rootPath, "sessions directory");
  } catch (error) {
    const selection = options.selection?.trim() ||
      process.env.NOEMANCER_EDITOR_SESSION?.trim();
    if (selection !== undefined && selection.length > 0) throw error;
    return [];
  }
  const candidates = await descriptorCandidates(
    sessionsRoot,
    options.maxDescriptorBytes ?? defaultDescriptorBytes,
  );
  const selections: LiveSessionSelection[] = [];
  for (const descriptorPath of candidates) {
    try {
      selections.push(await selectionFromDescriptorPath(sessionsRoot, descriptorPath, options));
    } catch {
      // Native discovery reports malformed or unavailable credentials as
      // diagnostics rather than sessions.  Never connect through one.
    }
  }
  return selections;
}

function validatedTimeout(value: number | undefined, label: string): number {
  const timeout = value ?? defaultTimeoutMs;
  if (!Number.isSafeInteger(timeout) || timeout <= 0 || timeout > 60_000) {
    throw new Error(label + " is outside the allowed range.");
  }
  return timeout;
}

function validatedFrameLimit(value: number | undefined): number {
  const limit = value ?? defaultFrameBytes;
  if (!Number.isSafeInteger(limit) || limit < 1024 || limit > 8 * 1024 * 1024) {
    throw new Error("Live session frame limit is outside the allowed range.");
  }
  return limit;
}

function credentialToken(text: string): string {
  const token = text.trim();
  if (token.length === 0 || /[\u0000-\u001f\u007f]/u.test(token)) {
    throw new Error("Session credential is empty or contains control characters.");
  }
  return token;
}

export class LiveEditorSession {
  readonly descriptor: LiveSessionDescriptor;
  readonly descriptorPath: string;
  readonly sessionsRoot: string;
  #socket: Socket | undefined;
  #buffer = "";
  #nextRequestId = 0;
  #closed = false;
  #helloComplete = false;
  #pending = new Map<string, PendingRequest>();
  #helloWaiter: HelloWaiter | undefined;
  #maxFrameBytes: number;
  #defaultTimeoutMs: number;

  private constructor(
    selection: LiveSessionSelection,
    socket: Socket,
    maxFrameBytes: number,
    defaultTimeoutMs: number,
  ) {
    this.descriptor = selection.descriptor;
    this.descriptorPath = selection.descriptorPath;
    this.sessionsRoot = selection.sessionsRoot;
    this.#socket = socket;
    this.#maxFrameBytes = maxFrameBytes;
    this.#defaultTimeoutMs = defaultTimeoutMs;
    socket.setEncoding("utf8");
    socket.on("data", (chunk: string | Buffer) => {
      this.#onData(typeof chunk === "string" ? chunk : chunk.toString("utf8"));
    });
    socket.on("error", () => {
      this.#fail(new Error("Editor live session transport failed."));
    });
    socket.on("close", () => {
      this.#fail(new Error("Editor live session disconnected."));
    });
  }

  static async connect(
    selection: LiveSessionSelection,
    token: string,
    options: LiveSessionOptions = {},
  ): Promise<LiveEditorSession> {
    const timeoutMs = validatedTimeout(options.timeoutMs, "Live session timeout");
    const maxFrameBytes = validatedFrameLimit(options.maxFrameBytes);
    const safeToken = credentialToken(token);
    const socket = await new Promise<Socket>((resolvePromise, rejectPromise) => {
      const connection = connectSocket(selection.descriptor.endpoint);
      let settled = false;
      const timer = setTimeout(() => {
        if (settled) return;
        settled = true;
        connection.destroy();
        rejectPromise(new Error("Timed out connecting to the selected editor session."));
      }, timeoutMs);
      connection.once("connect", () => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        resolvePromise(connection);
      });
      connection.once("error", () => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        rejectPromise(new Error("Could not connect to the selected editor session."));
      });
    });

    const session = new LiveEditorSession(selection, socket, maxFrameBytes, timeoutMs);
    try {
      await session.#hello(safeToken);
      return session;
    } catch (error) {
      await session.close();
      throw error;
    }
  }

  async invoke(
    name: string,
    arguments_: unknown,
    options: LiveSessionInvokeOptions = {},
  ): Promise<unknown> {
    if (this.#closed || !this.#helloComplete || this.#socket === undefined) {
      throw new Error("Editor live session is not connected.");
    }
    if (typeof name !== "string" || name.length === 0 || name.length > maxMethodBytes ||
        /[\r\n]/u.test(name)) {
      throw new Error("Live session tool name is invalid.");
    }
    const timeoutMs = validatedTimeout(options.timeoutMs, "Live session request timeout");
    const requestId = "request-" + (++this.#nextRequestId).toString(10);
    const deadlineUnixMs = Date.now() + timeoutMs;
    let frame: string;
    try {
      frame = JSON.stringify({
        type: "invoke",
        protocol: transportProtocol,
        requestId,
        method: name,
        arguments: arguments_,
        timeoutMs,
        deadlineUnixMs,
      });
    } catch {
      throw new Error("Live session arguments are not JSON serializable.");
    }
    return new Promise<unknown>((resolvePromise, rejectPromise) => {
      const timer = setTimeout(() => {
        this.#pending.delete(requestId);
        rejectPromise(new Error("Editor live session request timed out."));
      }, timeoutMs);
      this.#pending.set(requestId, { resolve: resolvePromise, reject: rejectPromise, timer });
      try {
        this.#writeFrame(frame);
      } catch (error) {
        this.#pending.delete(requestId);
        clearTimeout(timer);
        rejectPromise(error instanceof Error ? error : new Error("Editor live session write failed."));
      }
    });
  }

  async close(): Promise<void> {
    if (this.#closed) return;
    this.#closed = true;
    this.#rejectPending(new Error("Editor live session closed."));
    const socket = this.#socket;
    this.#socket = undefined;
    if (socket === undefined) return;
    await new Promise<void>((resolvePromise) => {
      let settled = false;
      const finish = () => {
        if (settled) return;
        settled = true;
        resolvePromise();
      };
      socket.once("close", finish);
      socket.end();
      setTimeout(() => {
        socket.destroy();
        finish();
      }, 250);
    });
  }

  #writeFrame(frame: string): void {
    const bytes = Buffer.byteLength(frame, "utf8") + 1;
    if (bytes > this.#maxFrameBytes) {
      throw new Error("Live session frame exceeds the bounded size limit.");
    }
    const socket = this.#socket;
    if (this.#closed || socket === undefined || socket.destroyed) {
      throw new Error("Editor live session is not connected.");
    }
    try {
      socket.write(frame + "\n", "utf8");
    } catch {
      const error = new Error("Editor live session write failed.");
      this.#fail(error);
      throw error;
    }
  }

  #hello(token: string): Promise<void> {
    return new Promise<void>((resolvePromise, rejectPromise) => {
      let timer: NodeJS.Timeout;
      const waiter: HelloWaiter = {
        resolve: (message) => {
          void message;
          clearTimeout(timer);
          if (this.#helloWaiter === waiter) this.#helloWaiter = undefined;
          resolvePromise();
        },
        reject: (error) => {
          clearTimeout(timer);
          if (this.#helloWaiter === waiter) this.#helloWaiter = undefined;
          rejectPromise(error);
        },
      };
      this.#helloWaiter = waiter;
      timer = setTimeout(() => {
        const error = new Error("Editor live session hello timed out.");
        if (this.#helloWaiter === waiter) this.#helloWaiter = undefined;
        rejectPromise(error);
        this.#fail(error);
      }, this.#defaultTimeoutMs);
      try {
        this.#writeFrame(JSON.stringify({
          type: "hello",
          protocol: transportProtocol,
          requestId: "hello",
          token,
        }));
      } catch (error) {
        waiter.reject(error instanceof Error ? error : new Error("Editor live session hello failed."));
      }
    });
  }

  #onData(chunk: string): void {
    if (this.#closed) return;
    this.#buffer += chunk;
    while (true) {
      const newline = this.#buffer.indexOf("\n");
      if (newline < 0) {
        if (Buffer.byteLength(this.#buffer, "utf8") > this.#maxFrameBytes) {
          this.#fail(new Error("Editor live session stream exceeded the bounded frame limit."));
        }
        return;
      }
      const line = this.#buffer.slice(0, newline).replace(/\r$/u, "");
      this.#buffer = this.#buffer.slice(newline + 1);
      if (line.length === 0) continue;
      if (Buffer.byteLength(line, "utf8") > this.#maxFrameBytes) {
        this.#fail(new Error("Editor live session frame is too large."));
        return;
      }
      let message: unknown;
      try {
        message = JSON.parse(line);
      } catch {
        this.#fail(new Error("Editor live session returned invalid JSONL."));
        return;
      }
      if (!isObject(message)) {
        this.#fail(new Error("Editor live session returned a non-object frame."));
        return;
      }
      if (!this.#helloComplete) {
        if (stringField(message, "type") !== "hello" ||
            message.protocol !== transportProtocol ||
            message.requestId !== "hello" ||
            message.ok !== true ||
            message.error !== undefined) {
          this.#fail(new Error("Editor live session hello was rejected."));
          return;
        }
        this.#helloComplete = true;
        const waiter = this.#helloWaiter;
        this.#helloWaiter = undefined;
        waiter?.resolve(message);
        continue;
      }
      if (stringField(message, "type") !== "response" ||
          message.protocol !== transportProtocol) {
        this.#fail(new Error("Editor live session returned an invalid response frame."));
        return;
      }
      const requestId = message.requestId;
      if (typeof requestId !== "string" && typeof requestId !== "number") continue;
      const key = String(requestId);
      const pending = this.#pending.get(key);
      if (pending === undefined) continue;
      this.#pending.delete(key);
      clearTimeout(pending.timer);
      if (message.ok !== true || message.error !== undefined) {
        pending.reject(new Error("Editor live session request failed."));
        continue;
      }
      if (Object.prototype.hasOwnProperty.call(message, "result")) {
        pending.resolve(message.result);
      } else {
        pending.resolve(message);
      }
    }
  }

  #rejectPending(error: Error): void {
    for (const pending of this.#pending.values()) {
      clearTimeout(pending.timer);
      pending.reject(error);
    }
    this.#pending.clear();
    const waiter = this.#helloWaiter;
    this.#helloWaiter = undefined;
    waiter?.reject(error);
  }

  #fail(error: Error): void {
    if (this.#closed) return;
    this.#closed = true;
    this.#rejectPending(error);
    this.#socket?.destroy();
    this.#socket = undefined;
  }
}

async function openSelectedLiveSession(
  selection: LiveSessionSelection,
  options: LiveSessionOptions,
): Promise<LiveEditorSession> {
  const token = credentialToken(await boundedRead(
    selection.credentialPath,
    options.maxCredentialBytes ?? defaultCredentialBytes,
    "session credential",
  ));
  return LiveEditorSession.connect(selection, token, options);
}

export async function openLiveEditorSession(
  options: LiveSessionOptions = {},
): Promise<LiveEditorSession> {
  const selection = await resolveLiveSession(options);
  return openSelectedLiveSession(selection, options);
}

/**
 * Prefer the one healthy running Editor when no explicit session was chosen.
 * A missing/empty session directory is the only case that returns undefined;
 * callers may then use their isolated serve fallback.  Existing descriptors
 * with zero or multiple healthy transports are surfaced as hard errors.
 */
export async function openPreferredLiveEditorSession(
  options: LiveSessionOptions = {},
): Promise<LiveEditorSession | undefined> {
  const explicit = options.selection?.trim() ||
    process.env.NOEMANCER_EDITOR_SESSION?.trim();
  if (explicit !== undefined && explicit.length > 0) {
    return openLiveEditorSession({ ...options, selection: explicit });
  }

  const selections = await discoverLiveSessions(options);
  if (selections.length === 0) return undefined;
  if (selections.length > maxHealthProbes) {
    throw new Error("Too many Editor sessions are available; select one explicitly.");
  }

  const configuredTimeout = validatedTimeout(
    options.timeoutMs,
    "Live session request timeout",
  );
  const probeOptions: LiveSessionOptions = {
    ...options,
    timeoutMs: Math.min(configuredTimeout, maxDiscoveryProbeMs),
  };
  const probeResults = await Promise.all(selections.map(async (selection) => {
    try {
      return await openSelectedLiveSession(selection, probeOptions);
    } catch {
      // A stale descriptor is not a running Editor. Keep probing the
      // bounded candidate set concurrently, while never exposing transport
      // or credential details.
      return undefined;
    }
  }));
  const healthy = probeResults.filter(
    (session): session is LiveEditorSession => session !== undefined,
  );
  if (healthy.length === 1) return healthy[0]!;
  if (healthy.length === 0) {
    throw new Error("Editor session descriptors exist, but no healthy running Editor session is available.");
  }
  await Promise.all(healthy.map((session) => session.close()));
  throw new Error("Multiple healthy running Editor sessions are available; select one explicitly.");
}
