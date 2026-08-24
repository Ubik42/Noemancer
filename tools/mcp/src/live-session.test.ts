import { test } from "node:test";
import assert from "node:assert/strict";
import { once } from "node:events";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { createServer, type Server, type Socket } from "node:net";
import { tmpdir } from "node:os";
import { join } from "node:path";
import {
  discoverLiveSessions,
  openLiveEditorSession,
  openPreferredLiveEditorSession,
  resolveLiveSession,
  type LiveEditorSession,
} from "./live-session.js";

type FixtureMode = "reply" | "disconnect" | "hang";

type Fixture = {
  readonly root: string;
  readonly server: Server;
  readonly endpoint: string;
};

let pipeSequence = 0;

function endpointFor(root: string): string {
  if (process.platform === "win32") {
    pipeSequence += 1;
    return "\\\\.\\pipe\\noemancer-live-session-test-" +
      process.pid.toString(10) + "-" + pipeSequence.toString(10);
  }
  return join(root, "editor.sock");
}

async function writeDescriptor(
  root: string,
  sessionId: string,
  endpoint: string,
  token = "test-token",
  credentialFile = sessionId + ".credential",
): Promise<void> {
  await writeFile(join(root, credentialFile), token + "\n", "utf8");
  await writeFile(join(root, sessionId + ".json"), JSON.stringify({
    schemaVersion: "noemancer.live-editor-session/0.1",
    version: 1,
    sessionId,
    processId: 1234,
    processIdentity: "test-process",
    projectId: "test-project",
    projectName: "Live session test",
    endpoint,
    credentialFile,
    capabilities: ["editor.observe", "editor.invoke"],
    createdUnixMilliseconds: 1,
    heartbeatUnixMilliseconds: 1,
    revision: 1,
  }), "utf8");
}

async function makeFixture(mode: FixtureMode): Promise<Fixture> {
  const root = await mkdtemp(join(tmpdir(), "noemancer-live-session-test-"));
  const endpoint = endpointFor(root);
  const server = createServer((socket: Socket) => {
    socket.setEncoding("utf8");
    let buffer = "";
    let helloSeen = false;
    socket.on("data", (chunk: string | Buffer) => {
      buffer += typeof chunk === "string" ? chunk : chunk.toString("utf8");
      while (true) {
        const newline = buffer.indexOf("\n");
        if (newline < 0) return;
        const line = buffer.slice(0, newline);
        buffer = buffer.slice(newline + 1);
        if (line.length === 0) continue;
        const message = JSON.parse(line) as {
          type?: string;
          protocol?: string;
          token?: string;
          requestId?: string | number;
          method?: string;
          timeoutMs?: number;
          deadlineUnixMs?: number;
          name?: string;
          arguments?: unknown;
        };
        if (!helloSeen) {
          assert.equal(message.type, "hello");
          assert.equal(message.protocol, "noemancer.live-editor/0.1");
          assert.equal(message.requestId, "hello");
          assert.equal(message.token, "test-token");
          helloSeen = true;
          socket.write(JSON.stringify({
            type: "hello",
            protocol: "noemancer.live-editor/0.1",
            requestId: "hello",
            ok: true,
          }) + "\n");
          continue;
        }
        assert.equal(message.type, "invoke");
        assert.equal(message.protocol, "noemancer.live-editor/0.1");
        assert.match(String(message.requestId), /^request-[0-9]+$/u);
        assert.equal(typeof message.method, "string");
        assert.equal(typeof message.timeoutMs, "number");
        assert.ok(typeof message.deadlineUnixMs === "number");
        if (mode === "disconnect") {
          socket.destroy();
          return;
        }
        if (mode === "hang") return;
        socket.write(JSON.stringify({
          type: "response",
          protocol: "noemancer.live-editor/0.1",
          requestId: message.requestId,
          ok: true,
          result: { method: message.method, arguments: message.arguments },
        }) + "\n");
      }
    });
  });
  server.listen(endpoint);
  await once(server, "listening");
  await writeDescriptor(root, "editor-test", endpoint);
  return { root, server, endpoint };
}

async function closeServer(server: Server): Promise<void> {
  if (!server.listening) return;
  await new Promise<void>((resolvePromise) => {
    server.close(() => resolvePromise());
  });
}

async function disposeFixture(fixture: Fixture): Promise<void> {
  await closeServer(fixture.server);
  await rm(fixture.root, { recursive: true, force: true });
}

test("connects to one selected session and invokes over JSONL", async () => {
  const fixture = await makeFixture("reply");
  let session: LiveEditorSession | undefined;
  try {
    session = await openLiveEditorSession({
      sessionsRoot: fixture.root,
      selection: "editor-test",
      timeoutMs: 1_000,
    });
    const response = await session.invoke("editor.observe", { scope: "selection" });
    assert.deepEqual(response, {
      method: "editor.observe",
      arguments: { scope: "selection" },
    });
    assert.equal(session.descriptor.sessionId, "editor-test");
  } finally {
    await session?.close();
    await disposeFixture(fixture);
  }
});

test("discovers the one healthy Editor and rejects multiple healthy Editors", async () => {
  const fixture = await makeFixture("reply");
  let session: LiveEditorSession | undefined;
  try {
    const discovered = await discoverLiveSessions({ sessionsRoot: fixture.root });
    assert.equal(discovered.length, 1);
    session = await openPreferredLiveEditorSession({
      sessionsRoot: fixture.root,
      timeoutMs: 1_000,
    });
    assert.ok(session);
  } finally {
    await session?.close();
    await disposeFixture(fixture);
  }

  const first = await makeFixture("reply");
  const second = await makeFixture("reply");
  let firstSession: LiveEditorSession | undefined;
  let secondSession: LiveEditorSession | undefined;
  try {
    // Keep both descriptors in one root while retaining independent servers.
    await writeDescriptor(first.root, "editor-second", second.endpoint);
    await writeFile(
      join(first.root, "editor-second.credential"),
      "test-token\n",
      "utf8",
    );
    await assert.rejects(
      openPreferredLiveEditorSession({
        sessionsRoot: first.root,
        timeoutMs: 1_000,
      }),
      /Multiple healthy running Editor sessions/u,
    );
  } finally {
    await firstSession?.close();
    await secondSession?.close();
    await disposeFixture(first);
    await disposeFixture(second);
  }
});

test("rejects ambiguous discovery instead of selecting an arbitrary editor", async () => {
  const root = await mkdtemp(join(tmpdir(), "noemancer-live-session-ambiguous-"));
  try {
    await writeDescriptor(root, "editor-a", "\\\\.\\pipe\\unused-editor-a");
    await writeDescriptor(root, "editor-b", "\\\\.\\pipe\\unused-editor-b");
    await assert.rejects(
      resolveLiveSession({ sessionsRoot: root }),
      /Multiple editor sessions are available/u,
    );
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});

test("rejects credential traversal and descriptor paths outside the sessions root", async () => {
  const root = await mkdtemp(join(tmpdir(), "noemancer-live-session-boundary-"));
  const outsideRoot = await mkdtemp(join(tmpdir(), "noemancer-live-session-outside-"));
  try {
    await writeFile(join(outsideRoot, "outside.credential"), "do-not-use\n", "utf8");
    await writeFile(join(root, "editor-test.json"), JSON.stringify({
      schemaVersion: "noemancer.live-editor-session/0.1",
      sessionId: "editor-test",
      endpoint: "\\\\.\\pipe\\unused-editor",
      credentialFile: "../outside.credential",
    }), "utf8");
    await assert.rejects(
      resolveLiveSession({ sessionsRoot: root, selection: "editor-test" }),
      /root-relative safe filename/u,
    );

    await writeDescriptor(outsideRoot, "outside-editor", "\\\\.\\pipe\\unused-editor");
    await assert.rejects(
      resolveLiveSession({
        sessionsRoot: root,
        selection: join(outsideRoot, "outside-editor.json"),
      }),
      /outside the sessions directory/u,
    );
  } finally {
    await rm(root, { recursive: true, force: true });
    await rm(outsideRoot, { recursive: true, force: true });
  }
});

test("does not expose a token embedded in a malformed descriptor", async () => {
  const root = await mkdtemp(join(tmpdir(), "noemancer-live-session-secret-"));
  try {
    await writeFile(join(root, "editor-test.json"), JSON.stringify({
      schemaVersion: "noemancer.live-editor-session/0.1",
      sessionId: "editor-test",
      endpoint: "\\\\.\\pipe\\unused-editor",
      credentialFile: "editor-test.credential",
      token: "secret-that-must-not-escape",
    }), "utf8");
    await writeFile(join(root, "editor-test.credential"), "test-token\n", "utf8");
    await assert.rejects(
      resolveLiveSession({ sessionsRoot: root, selection: "editor-test" }),
      (error: unknown) => {
        assert.ok(error instanceof Error);
        assert.match(error.message, /Bearer credentials must remain/u);
        assert.doesNotMatch(error.message, /secret-that-must-not-escape/u);
        return true;
      },
    );
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});

test("rejects pending invocation when the live editor disconnects", async () => {
  const fixture = await makeFixture("disconnect");
  let session: LiveEditorSession | undefined;
  try {
    session = await openLiveEditorSession({
      sessionsRoot: fixture.root,
      selection: "editor-test",
      timeoutMs: 1_000,
    });
    await assert.rejects(
      session.invoke("editor.mutate", { value: 1 }, { timeoutMs: 1_000 }),
      /disconnected|transport|closed/u,
    );
  } finally {
    await session?.close();
    await disposeFixture(fixture);
  }
});

test("bounds an invocation that receives no response", async () => {
  const fixture = await makeFixture("hang");
  let session: LiveEditorSession | undefined;
  try {
    session = await openLiveEditorSession({
      sessionsRoot: fixture.root,
      selection: "editor-test",
      timeoutMs: 1_000,
    });
    await assert.rejects(
      session.invoke("editor.observe", {}, { timeoutMs: 80 }),
      /timed out/u,
    );
  } finally {
    await session?.close();
    await disposeFixture(fixture);
  }
});
