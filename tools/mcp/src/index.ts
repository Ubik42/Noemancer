import { spawn } from "node:child_process";
import { readFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { createInterface } from "node:readline";
import { fileURLToPath } from "node:url";

import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { fromJSONSchema } from "zod";

import {
  openPreferredLiveEditorSession,
  type LiveEditorSession,
} from "./live-session.js";

const currentDirectory = dirname(fileURLToPath(import.meta.url));
const repositoryRoot = resolve(currentDirectory, "../../..");
const engineExecutable = resolve(
  repositoryRoot,
  "build/windows-msvc-debug/src/runtime/Debug/noemancer.exe",
);
const attachedProject = process.env.NOEMANCER_PROJECT?.trim();

type ToolDescriptor = {
  name: string;
  description: string;
  access: "read" | "write" | "control" | "debug";
  idempotent: boolean;
  inputSchema: Parameters<typeof fromJSONSchema>[0];
};

type ToolManifest = {
  protocolVersion: string;
  tools: ToolDescriptor[];
};

type SessionReply = {
  id: number;
  exitCode: number;
  response: unknown;
};

async function runEngine(
  arguments_: readonly string[],
  stdin?: string,
): Promise<string> {
  return new Promise((resolvePromise, rejectPromise) => {
    const child = spawn(engineExecutable, [...arguments_], {
      cwd: repositoryRoot,
      shell: false,
      windowsHide: true,
    });

    let stdout = "";
    let stderr = "";
    child.stdout.setEncoding("utf8");
    child.stderr.setEncoding("utf8");
    child.stdout.on("data", (chunk: string) => { stdout += chunk; });
    child.stderr.on("data", (chunk: string) => { stderr += chunk; });
    child.on("error", rejectPromise);
    child.on("close", (code) => {
      if (code === 0) {
        resolvePromise(stdout.trim());
        return;
      }
      rejectPromise(new Error(`Engine exited with ${code}: ${stderr.trim()}`));
    });
    if (stdin === undefined) {
      child.stdin.end();
    } else {
      child.stdin.end(stdin, "utf8");
    }
  });
}

class EngineSession {
  readonly #child = spawn(engineExecutable, [
    "serve",
    ...(attachedProject === undefined || attachedProject.length === 0
      ? []
      : ["--project", attachedProject]),
    "--format",
    "jsonl",
  ], {
    cwd: repositoryRoot,
    shell: false,
    windowsHide: true,
  });
  readonly #pending = new Map<number, {
    resolve: (value: string) => void;
    reject: (reason: Error) => void;
  }>();
  #nextId = 0;
  #stderr = "";

  constructor() {
    this.#child.stderr.setEncoding("utf8");
    this.#child.stderr.on("data", (chunk: string) => { this.#stderr += chunk; });
    const lines = createInterface({ input: this.#child.stdout });
    lines.on("line", (line) => {
      try {
        const reply = JSON.parse(line) as SessionReply;
        const pending = this.#pending.get(reply.id);
        if (pending === undefined) return;
        this.#pending.delete(reply.id);
        if (reply.exitCode === 0) {
          pending.resolve(JSON.stringify(reply.response));
        } else {
          pending.reject(new Error(JSON.stringify(reply.response)));
        }
      } catch (error) {
        for (const pending of this.#pending.values()) {
          pending.reject(error instanceof Error ? error : new Error(String(error)));
        }
        this.#pending.clear();
      }
    });
    this.#child.on("close", (code) => {
      const error = new Error(`Persistent engine session exited with ${code}: ${this.#stderr.trim()}`);
      for (const pending of this.#pending.values()) pending.reject(error);
      this.#pending.clear();
    });
  }

  invoke(name: string, arguments_: unknown): Promise<string> {
    const id = ++this.#nextId;
    return new Promise((resolvePromise, rejectPromise) => {
      this.#pending.set(id, { resolve: resolvePromise, reject: rejectPromise });
      this.#child.stdin.write(`${JSON.stringify({ id, name, arguments: arguments_ })}\n`);
    });
  }
}

class LiveEditorEngineSession {
  readonly #session: LiveEditorSession;

  constructor(session: LiveEditorSession) {
    this.#session = session;
  }

  async invoke(name: string, arguments_: unknown): Promise<string> {
    const result = await this.#session.invoke(name, arguments_);
    if (typeof result === "string") return result;
    return JSON.stringify(result) ?? "null";
  }

  async close(): Promise<void> {
    await this.#session.close();
  }
}

const server = new McpServer({
  name: "noemancer",
  version: "0.2.0",
});

server.registerResource(
  "architecture",
  "noemancer://docs/architecture",
  {
    title: "Noemancer architecture",
    description: "Module boundaries and control-plane rules.",
    mimeType: "text/markdown",
  },
  async (uri) => ({
    contents: [{
      uri: uri.href,
      mimeType: "text/markdown",
      text: await readFile(resolve(repositoryRoot, "docs/architecture.md"), "utf8"),
    }],
  }),
);

const manifest = JSON.parse(
  await runEngine(["tools", "list", "--format", "json"]),
) as ToolManifest;
const selectedEditorSession = await openPreferredLiveEditorSession();
const engineSession = selectedEditorSession === undefined
  ? new EngineSession()
  : new LiveEditorEngineSession(selectedEditorSession);

if (!Array.isArray(manifest.tools)) {
  throw new Error("Engine tool manifest is missing the tools array");
}

for (const tool of manifest.tools) {
  const inputSchema = fromJSONSchema(tool.inputSchema);
  server.registerTool(
    tool.name,
    {
      description: tool.description,
      inputSchema,
      annotations: {
        readOnlyHint: tool.access === "read",
        idempotentHint: tool.idempotent,
        destructiveHint: tool.access !== "read",
      },
    },
    async (arguments_) => ({
      content: [{
        type: "text",
        text: await engineSession.invoke(tool.name, arguments_),
      }],
    }),
  );
}

const transport = new StdioServerTransport();
await server.connect(transport);
