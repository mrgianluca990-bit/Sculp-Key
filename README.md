# Sculpt Key — 0.1.1

Strumento virtuale per macOS VST3, con l'interfaccia di Sculpt Bass/Drums e una manopola principale:

**Piano → Rhodes → Hammond → Synth**

Questa revisione rende il Rhodes più caldo e adatto al jazz e l'organo più pieno e incisivo per il rock. Il pianoforte approvato e il Synth conservano il suono precedente. Il pacchetto contiene il progetto completo e la libreria; la compilazione macOS produce `Sculpt Key.vst3`.

## Suoni e controlli

| Morph | Suono |
| --- | --- |
| 0% | Pianoforte acustico stereo, campionato su tre livelli di dinamica |
| Circa 33,3% | Rhodes jazz: corpo caldo e rotondo, attacco meno metallico e grinta progressiva sulle note più forti |
| Circa 66,7% | Hammond rock: registrazione più piena, attacco e armoniche più incisivi, movimento stereo |
| 100% | Synth polifonico, con oscillatori limitati in banda, filtro e inviluppo |

I passaggi sono continui. Una nota già tenuta conserva la propria evoluzione mentre si gira la manopola: i timbri non ripartono dal proprio attacco. La manopola piccola regola il volume d'uscita. Doppio clic sul morph: piano; doppio clic sul volume: 0 dB.

La tastiera è polifonica: 48 voci, più 8 brevi code per raccordare le sostituzioni quando si supera il limite. Velocity, pedale sustain MIDI CC64 e pitch bend ±2 semitoni sono gestiti per canale MIDI. I parametri morph e volume si salvano nella sessione della DAW e sono automatizzabili.

Il round robin è automatico, con quattro variazioni leggere e deterministiche di attacco/timbro. **Sono variazioni modellate: la banca del piano contiene una ripresa registrata per nota campionata e livello dinamico, non quattro riprese indipendenti.** Non vengono aggiunti ritardi MIDI o scordature casuali.

## Pianoforte incluso

90 campioni WAV stereo a 44,1 kHz/16 bit: 30 note di riferimento da A0 a C8, ogni tre semitoni, con tre livelli dinamici. Provengono da Salamander Grand Piano v3, Yamaha C5 registrato da Alexander Holm, licenza CC BY 3.0. Le note intermedie vengono trasposte dalla root più vicina.

Il campione mantiene il decadimento originale, con code di durata massima 10/8/6 secondi per bassi/medi/alti e raccordo finale. Non si usa un loop artificiale del pianoforte. Non sono inclusi i layer separati di risonanza simpatetica, rumore del pedale o release della libreria completa. Crediti, licenza e trasformazioni sono descritti in `ThirdParty/Salamander` e `Samples/source-info.json`.

I WAV sono già inclusi e incorporati nel plugin durante la compilazione: il VST3 non richiede cartelle di campioni esterne. Lo script di compilazione conserva anche crediti e licenze nelle risorse del bundle VST3, prima della firma.

## Anteprima

Aprire `Preview/SculptKey-demo.wav`. La stessa frase viene eseguita sui quattro punti della manopola; segue un accordo tenuto durante il passaggio completo. La demo viene renderizzata dal medesimo motore C++ usato nel plugin, senza riverbero esterno e senza normalizzare separatamente i timbri. I tempi sono in `Preview/ASCOLTO.txt`.

## Compilazione su GitHub

1. Creare il repository **Sculpt-Key** e caricare il contenuto di questa cartella nella radice del repository. `CMakeLists.txt`, `Source`, `Samples` e `tools` devono trovarsi direttamente nella radice, non dentro una seconda cartella `SculptKey`.
2. Includere anche `.github/workflows/build-macos-vst3.yml`. La cartella `.github` è nascosta nel Finder: **Cmd + Shift + .** mostra i file nascosti. Se il caricamento dal browser la omette, usare **Add file → Create new file**, scrivere esattamente `.github/workflows/build-macos-vst3.yml` e incollare il contenuto del file fornito nel pacchetto.
3. Salvare sul branch `main`. Il workflow **Build macOS VST3** parte automaticamente; è disponibile anche **Actions → Build macOS VST3 → Run workflow**.
4. Quando la build è verde, scaricare l'artifact **Sculpt-Key-macOS-VST3**. Contiene lo ZIP con il bundle **Sculpt Key.vst3**.

Il workflow usa `macos-15`, compila Apple Silicon e Intel, verifica le due architetture con `lipo`, applica una firma ad hoc e crea lo ZIP. È già incluso l'ordine corretto degli argomenti di `lipo`.

## Compilazione locale su Mac

Servono Xcode Command Line Tools e CMake 3.22 o successivo. Dalla cartella del progetto:

```sh
bash tools/build-macos.sh
```

Output: `dist/Sculpt-Key-macOS-VST3.zip`. Il plugin è universale Apple Silicon/Intel, con target minimo macOS 11. Lo script non installa file nelle cartelle della DAW.

Per l'installazione, estrarre **Sculpt Key.vst3** e copiarlo in `~/Library/Audio/Plug-Ins/VST3`, quindi eseguire la scansione dei plugin nella DAW che ospita VST3. Questo progetto produce soltanto VST3.

## Verifiche e riproducibilità

```sh
bash tools/test-core.sh
```

I test del motore sono indipendenti da JUCE e richiedono un compilatore C++17. Il report della consegna è in `docs/VERIFICA.txt`; distingue i controlli eseguiti dalla build macOS e dalla prova in DAW ancora necessarie.

Per rigenerare l'anteprima dal motore:

```sh
mkdir -p build-preview
c++ -std=c++17 -O2 tools/render_preview.cpp -o build-preview/render-preview
build-preview/render-preview Samples Preview/SculptKey-demo.wav
```

L'importatore `tools/import_factory_samples.py` serve solo a rigenerare i WAV da un commit fissato della sorgente, verificandone gli hash. Non serve per la normale compilazione, perché i campioni sono già presenti. Per la rigenerazione servono Python, NumPy, SciPy e FFmpeg.

JUCE 8.0.4 viene scaricato da CMake alla prima compilazione; la sua licenza originale è conservata in `ThirdParty/JUCE-8.0.4-LICENSE.md`. Questo progetto non è affiliato ai produttori degli strumenti citati: Rhodes e Hammond indicano i timbri ricercati.
