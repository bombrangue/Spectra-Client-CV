import { spawn, ChildProcessWithoutNullStreams } from "child_process";
import path from "path";
import log from "electron-log";
import { gepService } from "../main";

function logWithTime(message: string) {
    const now = new Date();
    const ms = now.getMilliseconds().toString().padStart(3, '0');
    const timeStr = `${now.getHours().toString().padStart(2, '0')}:${now.getMinutes().toString().padStart(2, '0')}:${now.getSeconds().toString().padStart(2, '0')}.${ms}`;
    log.info(`[${timeStr}] ${message}`);
}

export class CVService {
  private static instance: CVService;
  private cvProcess: ChildProcessWithoutNullStreams | null = null;
  private currentCVState: any = {};
  private currentAgent: string = "";
  private currentPhase: string = "combat";
  private isConnected: boolean = false;
  private currentMode: "OFF" | "MAIN" | "AUX" = "OFF";

  private constructor() {
    this.startCVProcess();
  }

  public static getInstance(): CVService {
    if (!CVService.instance) {
      CVService.instance = new CVService();
    }
    return CVService.instance;
  }

  private startCVProcess() {
    if (this.cvProcess) {
      return;
    }

    const exePath = path.join(
      __dirname,
      "../../Spectra-CV-C++/out/build/x64-Debug/SpectraCV.exe"
    );

    log.info(`Starting Spectra CV process at: ${exePath}`);

    try {
      this.cvProcess = spawn(exePath, [], {
        cwd: path.dirname(exePath),
      });

      this.isConnected = true;

      this.cvProcess.stdout.on("data", (data) => {
        const lines = data.toString().split("\n");
        for (let line of lines) {
          line = line.trim();
          if (!line) continue;

          try {
            const parsed = JSON.parse(line);
            if (parsed.type === "state_update" && parsed.data) {
              logWithTime(`PLAYER Client received state from Spectra CV: ${JSON.stringify(parsed.data)}`);
              this.currentCVState = parsed.data;
            } else if (parsed.type === "gep_info" && parsed.data) {
              logWithTime(`Spectra CV mimicking GEP Info: ${JSON.stringify(parsed.data)}`);
              if (gepService && this.currentMode === "MAIN") {
                gepService.processInfoUpdate(parsed.data);
              }
            } else if (parsed.info) {
              logWithTime(`Spectra CV Info: ${parsed.info}`);
            } else if (parsed.error) {
              logWithTime(`Spectra CV Error: ${parsed.error}`);
            } else if (parsed.warning) {
              logWithTime(`Spectra CV Warning: ${parsed.warning}`);
            } else if (line.startsWith("[DEBUG]")) {
              // Direct debug logs from C++
              logWithTime(`Spectra CV: ${line}`);
            }
          } catch (e) {
            // It might be a debug log or non-JSON output, just log it as debug if we care
            if (line.startsWith("[DEBUG]")) {
              logWithTime(`Spectra CV: ${line}`);
            }
          }
        }
      });

      this.cvProcess.stderr.on("data", (data) => {
        log.error(`Spectra CV Stderr: ${data.toString().trim()}`);
      });

      this.cvProcess.on("close", (code) => {
        log.info(`Spectra CV process exited with code ${code}`);
        this.isConnected = false;
        this.cvProcess = null;

        // Auto-restart after 5 seconds if it crashes
        setTimeout(() => {
          this.startCVProcess();
        }, 5000);
      });

    } catch (error) {
      log.error(`Failed to start Spectra CV process:`, error);
    }
  }

  public getCVState(): any {
    return this.currentCVState;
  }

  public setCVMode(mode: "OFF" | "MAIN" | "AUX") {
    if (this.currentMode === mode) return;
    this.currentMode = mode;
    logWithTime(`Setting Spectra CV Mode to: ${mode}`);
    if (this.cvProcess && this.cvProcess.stdin) {
      const cmd = {
        action: "set_mode",
        mode: this.currentMode
      };
      this.cvProcess.stdin.write(JSON.stringify(cmd) + "\n");
    }
  }

  public setAgent(agentName: string) {
    if (this.currentAgent === agentName) return;

    // Filter out camera/form agents (these are internal names usually sent by GEP)
    if (agentName.includes("Targeting") || agentName.includes("PossessableCamera")) {
      return;
    }

    this.currentAgent = agentName;

    if (this.cvProcess && this.cvProcess.stdin) {
      const cmd = {
        action: "set_agent",
        agent: this.currentAgent
      };
      logWithTime(`PLAYER Client sending agent change to Spectra CV: ${this.currentAgent}`);
      this.cvProcess.stdin.write(JSON.stringify(cmd) + "\n");
      
      // Reset state on agent change
      this.currentCVState = {};
    }
  }

  public setPhase(phase: string) {
    const safePhase = phase === "buy_phase" ? "buy_phase" : "combat";
    if (this.currentPhase === safePhase) return;

    this.currentPhase = safePhase;

    if (this.cvProcess && this.cvProcess.stdin) {
      const cmd = {
        action: "set_phase",
        phase: this.currentPhase
      };
      logWithTime(`PLAYER Client sending phase change to Spectra CV: ${this.currentPhase}`);
      this.cvProcess.stdin.write(JSON.stringify(cmd) + "\n");
    }
  }

  public stop() {
    if (this.cvProcess) {
      const cmd = { action: "stop" };
      this.cvProcess.stdin.write(JSON.stringify(cmd) + "\n");
      this.cvProcess.kill();
      this.cvProcess = null;
      this.isConnected = false;
    }
  }
}
