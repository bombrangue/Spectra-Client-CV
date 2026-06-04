import { Component, Input, OnInit } from "@angular/core";
import { FormsModule } from "@angular/forms";
import { BlockUI } from "primeng/blockui";
import { ToggleSwitchModule } from "primeng/toggleswitch";
import { BlockableDiv } from "../blockablediv/blockablediv.component";
import { LocalstorageService } from "../services/localstorage.service";
import { ElectronService } from "../services/electron.service";
import { DialogModule } from "primeng/dialog";
import { ButtonModule } from "primeng/button";

@Component({
  selector: "app-options",
  imports: [ToggleSwitchModule, BlockUI, BlockableDiv, FormsModule, DialogModule, ButtonModule],
  templateUrl: "./options.component.html",
  styleUrl: "./options.component.css",
})
export class OptionsComponent implements OnInit {
  data: ClientOptions = {
    minimizeToTray: false,
    runAtStartup: false,
    startMinimized: false,
    aux: false,
    cvMode: 'GEP',
  };

  showCvWarning = false;
  cvWarningTimer = 5;
  cvWarningInterval: any;
  pendingCvMode: boolean = false;

  private readonly storageKey = "appOptions";

  @Input({ required: false })
  isAux = false;

  constructor(
    private storage: LocalstorageService,
    private electron: ElectronService,
  ) {}

  ngOnInit(): void {
    const saved = this.storage.getItem<ClientOptions>(this.storageKey);
    if (saved) {
      this.data.minimizeToTray = saved.minimizeToTray ?? this.data.minimizeToTray;
      this.data.runAtStartup = saved.runAtStartup ?? this.data.runAtStartup;
      this.data.startMinimized = saved.startMinimized ?? this.data.startMinimized;
      this.data.cvMode = saved.cvMode ?? this.data.cvMode;
    } else {
      // Seed defaults
      this.save();
    }
    // Apply tray setting to Electron main
    this.electron.setTraySetting(this.data.minimizeToTray);
    // Push startup settings to main (in case they were restored)
    this.electron.setStartupSettings(
      this.data.runAtStartup,
      this.data.runAtStartup && this.data.startMinimized,
      this.data.runAtStartup && this.isAux,
    );
  }

  save() {
    // If runAtStartup turned off, also clear startMinimized
    if (!this.data.runAtStartup && this.data.startMinimized) {
      this.data.startMinimized = false;
    }
    this.storage.setItem(this.storageKey, this.data);
    // Forward tray setting to Electron main
    this.electron.setTraySetting(this.data.minimizeToTray);
    // Update startup settings in main (start minimized disabled)
    this.electron.setStartupSettings(
      this.data.runAtStartup,
      this.data.runAtStartup && this.data.startMinimized,
      this.data.runAtStartup && this.isAux,
    );
    // When saved directly, apply CV mode if not handled by dialog
    if (this.data.cvMode === 'GEP') {
      this.electron.setCVMode(this.isAux ? 'AUX' : 'OFF');
    } else {
      this.electron.setCVMode('MAIN');
    }
  }

  onCvModeChange(event: any) {
    // Revert the ngModel change temporarily
    const isCvSelected = event.checked;
    this.data.cvMode = isCvSelected ? 'GEP' : 'CV'; // Revert back until confirmed

    if (isCvSelected) {
      // User wants to switch to CV
      this.showCvWarning = true;
      this.cvWarningTimer = 5;
      this.pendingCvMode = true;

      this.cvWarningInterval = setInterval(() => {
        this.cvWarningTimer--;
        if (this.cvWarningTimer <= 0) {
          clearInterval(this.cvWarningInterval);
        }
      }, 1000);
    } else {
      // User switched back to GEP
      this.data.cvMode = 'GEP';
      this.save();
    }
  }

  confirmCvMode() {
    this.showCvWarning = false;
    this.data.cvMode = 'CV';
    this.save();
  }

  cancelCvMode() {
    this.showCvWarning = false;
    this.data.cvMode = 'GEP';
    clearInterval(this.cvWarningInterval);
  }
}

export type ClientOptions = {
  minimizeToTray: boolean;
  runAtStartup: boolean;
  startMinimized: boolean;
  aux: boolean;
  cvMode: 'GEP' | 'CV';
};
