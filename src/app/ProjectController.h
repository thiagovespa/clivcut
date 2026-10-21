// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/Clipboard.h"
#include "core/History.h"
#include "core/Model.h"
#include "engine/Exporter.h"
#include "engine/RenderCache.h"

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>


#include <atomic>
#include <memory>
#include <mutex>

class AudioOutput;
class QLockFile;
class QNetworkAccessManager;
class QNetworkReply;
class QQmlEngine;
class AudioRecorder;
class QJSEngine;

/**
 * Ponte entre o modelo (clv::Project) e a interface QML: edição, seleção,
 * cursor de reprodução, histórico, arquivos, importação e exportação.
 *
 * Tempos expostos ao QML são em µs (double; exato até 2^53).
 * Exposto ao QML como o singleton `Project` (registrado em main.cpp).
 */
class ProjectController : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Project)
    QML_SINGLETON

    Q_PROPERTY(QString projectName READ projectName WRITE setProjectName NOTIFY projectChanged)
    Q_PROPERTY(QString filePath READ filePath NOTIFY fileChanged)
    Q_PROPERTY(bool modified READ modified NOTIFY modifiedChanged)
    /// Clipes compostos abertos para edição, de fora para dentro: [{name}]. Vazio = timeline principal.
    Q_PROPERTY(QVariantList compoundPath READ compoundPath NOTIFY compoundPathChanged)
    Q_PROPERTY(int canvasWidth READ canvasWidth NOTIFY projectChanged)
    Q_PROPERTY(int canvasHeight READ canvasHeight NOTIFY projectChanged)
    Q_PROPERTY(double fps READ fps NOTIFY projectChanged)
    Q_PROPERTY(QString background READ background NOTIFY projectChanged)
    Q_PROPERTY(double backgroundBlur READ backgroundBlur NOTIFY projectChanged)
    Q_PROPERTY(double duration READ duration NOTIFY projectChanged)
    Q_PROPERTY(bool magnet READ magnet WRITE setMagnet NOTIFY projectChanged)
    Q_PROPERTY(bool snapping READ snapping WRITE setSnapping NOTIFY snappingChanged)
    Q_PROPERTY(QVariantList tracks READ tracks NOTIFY projectChanged)
    Q_PROPERTY(QVariantList assets READ assets NOTIFY assetsChanged)
    Q_PROPERTY(QVariantList markers READ markers NOTIFY projectChanged)
    /// Pontos de entrada/saída em µs (-1 = não definido).
    Q_PROPERTY(double inPoint READ inPoint NOTIFY projectChanged)
    Q_PROPERTY(double outPoint READ outPoint NOTIFY projectChanged)
    Q_PROPERTY(double playhead READ playhead WRITE seek NOTIFY playheadChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(QStringList selection READ selection NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap selectedClip READ selectedClip NOTIFY selectedClipChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(QString undoText READ undoText NOTIFY historyChanged)
    Q_PROPERTY(QString redoText READ redoText NOTIFY historyChanged)
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    Q_PROPERTY(double exportProgress READ exportProgress NOTIFY exportChanged)
    Q_PROPERTY(bool exporting READ exporting NOTIFY exportChanged)
    Q_PROPERTY(QStringList videoCodecs READ videoCodecs CONSTANT)
    /// Codecs com encoder de GPU funcional nesta máquina (sondado na primeira leitura).
    Q_PROPERTY(QStringList hardwareCodecs READ hardwareCodecs CONSTANT)
    Q_PROPERTY(QStringList audioFormats READ audioFormats CONSTANT)
    /// Lado maior da pré-visualização (960/1280/1920): define também a resolução do cache de renderização.
    Q_PROPERTY(int previewResolution READ previewResolution WRITE setPreviewResolution NOTIFY renderCacheChanged)
    /// Trechos já renderizados: [{start, end}] em µs.
    Q_PROPERTY(QVariantList cachedRanges READ cachedRanges NOTIFY renderCacheChanged)
    Q_PROPERTY(bool renderingPreview READ renderingPreview NOTIFY renderCacheChanged)
    Q_PROPERTY(double renderProgress READ renderProgress NOTIFY renderCacheChanged)
    /// Remoção de silêncios: pausas encontradas na última análise ([{start, end}] em µs).
    Q_PROPERTY(QVariantList silences READ silences NOTIFY silencesChanged)
    Q_PROPERTY(bool analyzingSilences READ analyzingSilences NOTIFY silencesChanged)
    Q_PROPERTY(double silenceProgress READ silenceProgress NOTIFY silencesChanged)
    /// Detecção de batidas da música em andamento; existem marcadores de batida.
    Q_PROPERTY(bool detectingBeats READ detectingBeats NOTIFY beatsChanged)
    /// Escolhendo a região a rastrear no player; progresso do rastreamento (-1 = parado).
    Q_PROPERTY(bool trackingPick READ trackingPick NOTIFY trackingChanged)
    Q_PROPERTY(double trackingProgress READ trackingProgress NOTIFY trackingChanged)
    Q_PROPERTY(bool hasBeatMarkers READ hasBeatMarkers NOTIFY projectChanged)
    /// Transcrição palavra a palavra da timeline: [{start, end, text}] em µs.
    Q_PROPERTY(QVariantList transcript READ transcript NOTIFY transcriptChanged)
    /// O projeto mudou por outra edição depois da transcrição: os tempos podem não bater.
    Q_PROPERTY(bool transcriptStale READ transcriptStale NOTIFY transcriptChanged)
    /// Escopos visíveis: o player passa a enviar quadros reduzidos (scopeFrame).
    Q_PROPERTY(bool scopesEnabled READ scopesEnabled WRITE setScopesEnabled NOTIFY scopesChanged)
    /// Efeitos de voz disponíveis: [{id, name}].
    Q_PROPERTY(QVariantList voiceEffects READ voiceEffects NOTIFY languageChanged)
    /// Efeitos de vídeo disponíveis: [{id, name}].
    Q_PROPERTY(QVariantList videoEffects READ videoEffects NOTIFY languageChanged)
    /// Stickers embutidos (Fluent Emoji, MIT): [{id, source}].
    Q_PROPERTY(QVariantList stickers READ stickers CONSTANT)
    /// Narração: gravando do microfone, nível (pico 0..1) e duração gravada em µs.
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
    Q_PROPERTY(double recordLevel READ recordLevel NOTIFY recordLevelChanged)
    Q_PROPERTY(double recordElapsed READ recordElapsed NOTIFY recordLevelChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    // Legendas automáticas
    Q_PROPERTY(bool transcriptionAvailable READ transcriptionAvailable CONSTANT)
    Q_PROPERTY(QVariantList captionModels READ captionModels NOTIFY captionModelsChanged)
    Q_PROPERTY(QString downloadingModel READ downloadingModel NOTIFY captionModelsChanged)
    Q_PROPERTY(double modelDownloadProgress READ modelDownloadProgress NOTIFY modelDownloadProgressChanged)
    Q_PROPERTY(bool transcribing READ transcribing NOTIFY transcribeChanged)
    Q_PROPERTY(double transcribeProgress READ transcribeProgress NOTIFY transcribeChanged)
    Q_PROPERTY(QVariantList audioTracks READ audioTracks NOTIFY projectChanged)
    Q_PROPERTY(QVariantList filters READ filters NOTIFY languageChanged)
    Q_PROPERTY(QVariantList speedPresets READ speedPresets NOTIFY languageChanged)
    /// Idioma escolhido ("" = do sistema) e o efetivamente em uso.
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)
    Q_PROPERTY(QString activeLanguage READ activeLanguage NOTIFY languageChanged)
    Q_PROPERTY(QVariantList languages READ languages CONSTANT)
    /// Aba inicial do inspetor para capturas automáticas (CLIVCUT_SCREENSHOT_PAGE).
    Q_PROPERTY(QString screenshotPage READ screenshotPage CONSTANT)
    Q_PROPERTY(bool useProxies READ useProxies WRITE setUseProxies NOTIFY proxiesChanged)
    Q_PROPERTY(int proxiesPending READ proxiesPending NOTIFY proxiesChanged)
    Q_PROPERTY(bool canPaste READ canPaste NOTIFY clipboardChanged)
    Q_PROPERTY(bool normalizing READ normalizing NOTIFY audioJobsChanged)
    Q_PROPERTY(QVariantList recentProjects READ recentProjects NOTIFY recentProjectsChanged)
    Q_PROPERTY(bool canPasteAttributes READ canPasteAttributes NOTIFY clipboardChanged)

public:
    /// Sem construtor padrão: o QML obtém a instância única via create().
    explicit ProjectController(QObject *parent);
    ~ProjectController() override;

    static ProjectController *instance();
    /// Fábrica do singleton QML: devolve a instância criada em main().
    static ProjectController *create(QQmlEngine *, QJSEngine *);

    QString language() const;
    void setLanguage(const QString &code);
    QString activeLanguage() const;
    QVariantList languages() const;

    /// Cópia imutável do projeto para outras threads (render, áudio).
    std::shared_ptr<const clv::Project> snapshot() const;

    QString projectName() const { return m_project.name; }
    void setProjectName(const QString &name);
    QString filePath() const { return m_filePath; }
    bool modified() const { return m_history.revision() != m_savedRevision; }
    int canvasWidth() const { return m_project.width; }
    int canvasHeight() const { return m_project.height; }
    double fps() const { return m_project.fps; }
    QString background() const { return m_project.background; }
    double backgroundBlur() const { return m_project.backgroundBlur; }
    double duration() const { return static_cast<double>(m_project.duration()); }
    bool magnet() const { return m_project.mainTrackMagnet; }
    void setMagnet(bool on);
    bool snapping() const { return m_snapping; }
    void setSnapping(bool on);
    QVariantList tracks() const;
    QVariantList assets() const;
    QVariantList markers() const;
    double playhead() const { return static_cast<double>(m_playhead); }
    bool playing() const { return m_playing; }
    QStringList selection() const { return m_selection; }
    QVariantMap selectedClip() const;
    bool canUndo() const { return m_history.canUndo(); }
    bool canRedo() const { return m_history.canRedo(); }
    QString undoText() const { return m_history.undoLabel(); }
    QString redoText() const { return m_history.redoLabel(); }
    int revision() const { return m_revisionCounter; }
    double exportProgress() const { return m_exportProgress; }
    bool exporting() const { return m_exporting; }
    QStringList videoCodecs() const;
    QStringList hardwareCodecs() const;
    int previewResolution() const { return m_previewResolution; }
    void setPreviewResolution(int side);
    QVariantList cachedRanges() const;
    bool renderingPreview() const { return m_rendering; }
    double renderProgress() const { return m_renderProgress; }
    QVariantList silences() const { return m_silences; }
    bool analyzingSilences() const { return m_analyzingSilences; }
    bool detectingBeats() const { return m_detectingBeats; }
    bool hasBeatMarkers() const;
    /// Marca as batidas do clipe selecionado (uma a cada `every`), trocando as marcas antigas dele.
    Q_INVOKABLE void detectBeats(int every = 1);
    Q_INVOKABLE void clearBeatMarkers();
    double silenceProgress() const { return m_silenceProgress; }
    QVariantList transcript() const { return m_transcript; }
    bool scopesEnabled() const { return m_scopesEnabled; }
    QVariantList voiceEffects() const;
    QVariantList videoEffects() const;
    /// Camada de efeito (ajuste) do cursor até o fim do clipe principal, com o efeito dado.
    Q_INVOKABLE void addEffectClip(const QString &effectId);
    QVariantList stickers() const;
    /// Coloca o sticker no cursor, numa faixa de cima, pequeno e centrado.
    Q_INVOKABLE void addSticker(const QString &id);
    bool recording() const;
    double recordLevel() const { return m_recordLevel; }
    double recordElapsed() const;
    /// Microfones disponíveis.
    Q_INVOKABLE QStringList inputDevices() const;
    /// Grava narração a partir do cursor enquanto a timeline toca; `device` vazio = padrão.
    Q_INVOKABLE bool startRecording(const QString &device = {});
    /// Para, importa o áudio e o coloca na timeline onde a gravação começou.
    Q_INVOKABLE void stopRecording();
    void setScopesEnabled(bool on);
    /// Chamado pela thread de renderização com o quadro composto reduzido.
    void setScopeFrame(const QImage &img);
    QImage scopeFrame() const;
    bool transcriptStale() const { return !m_transcript.isEmpty() && m_transcriptRevision != m_revisionCounter; }
    /// Estado do cache lido pela pré-visualização (thread de renderização).
    std::shared_ptr<const clv::engine::RenderCacheView> renderCacheView() const;
    QStringList audioFormats() const;
    QString statusMessage() const { return m_status; }
    bool transcriptionAvailable() const;
    QVariantList captionModels() const;
    QString downloadingModel() const { return m_downloadingModel; }
    double modelDownloadProgress() const { return m_modelDownloadProgress; }
    bool transcribing() const { return m_transcribing; }
    double transcribeProgress() const { return m_transcribeProgress; }
    QVariantList audioTracks() const;
    QVariantList filters() const;
    QVariantList speedPresets() const;
    QString screenshotPage() const { return qEnvironmentVariable("CLIVCUT_SCREENSHOT_PAGE"); }
    bool canPaste() const;
    bool normalizing() const { return m_normalizing; }
    QVariantList recentProjects() const;
    bool canPasteAttributes() const { return m_attributeSource.has_value(); }
    bool useProxies() const { return m_useProxies; }
    void setUseProxies(bool on);
    int proxiesPending() const { return m_proxyQueue.size() + (m_proxyBuilding.isEmpty() ? 0 : 1); }

    // --- arquivos ---
    Q_INVOKABLE void newProject(int width, int height, double fps);
    Q_INVOKABLE bool openProject(const QUrl &url);
    Q_INVOKABLE bool saveProject();
    Q_INVOKABLE bool saveProjectAs(const QUrl &url);
    Q_INVOKABLE bool importDraft(const QUrl &url);
    Q_INVOKABLE void importMedia(const QList<QUrl> &urls);
    Q_INVOKABLE void setCanvas(int width, int height);
    Q_INVOKABLE void setBackground(const QString &color);
    /// Intensidade do fundo desfocado (0 = desligado).
    Q_INVOKABLE void setBackgroundBlur(double amount);
    Q_INVOKABLE void relinkAsset(const QString &assetId, const QUrl &url);

    // --- recuperação ---
    /// Cópias de recuperação deixadas por sessões que não fecharam normalmente.
    Q_INVOKABLE QVariantList recoverableProjects() const;
    Q_INVOKABLE bool recoverProject(const QString &recoveryFile);
    Q_INVOKABLE void discardRecovery(const QString &recoveryFile);
    /// Grava a cópia de recuperação agora (se houver alterações).
    Q_INVOKABLE void autosaveNow();
    /// Abandona as alterações (ao fechar sem salvar): apaga a cópia de recuperação.
    Q_INVOKABLE void discardChanges();
    static QString recoveryDir();

    // --- timeline ---
    Q_INVOKABLE void addAssetToTimeline(const QString &assetId, int trackIndex = -1, double start = -1);
    Q_INVOKABLE void addText(const QString &text = {}, const QString &preset = {});
    /// Forma (retângulo, círculo, estrela, seta, coração, balão...) no cursor, numa faixa de texto.
    Q_INVOKABLE void addShape(const QString &kind);
    Q_INVOKABLE void splitAtPlayhead();
    Q_INVOKABLE void deleteSelection(bool ripple = false);
    Q_INVOKABLE void deleteLeft();
    Q_INVOKABLE void deleteRight();
    Q_INVOKABLE void duplicateSelection();
    Q_INVOKABLE bool moveClip(const QString &clipId, int trackIndex, double start);
    Q_INVOKABLE bool trimClip(const QString &clipId, bool startEdge, double time);
    Q_INVOKABLE double snapTime(double t, const QStringList &ignore, double thresholdUs) const;
    Q_INVOKABLE void setTrackFlag(int trackIndex, const QString &flag, bool value);
    Q_INVOKABLE void addMarker();
    /// Junta os clipes selecionados num clipe composto / devolve o conteúdo do composto à timeline.
    Q_INVOKABLE void createCompound();
    Q_INVOKABLE void breakCompound();
    /// Abre o conteúdo do composto na timeline / volta um nível (as mudanças viram um passo de desfazer).
    Q_INVOKABLE void openCompound(const QString &clipId);
    Q_INVOKABLE void closeCompound();
    QVariantList compoundPath() const;
    /// Marca entrada/saída no cursor (I/O); limpar com clearInOut (Alt+X).
    Q_INVOKABLE void markIn();
    Q_INVOKABLE void markOut();
    Q_INVOKABLE void clearInOut();
    Q_INVOKABLE void jumpToInOut(bool out);
    double inPoint() const { return static_cast<double>(m_project.inPoint); }
    double outPoint() const { return static_cast<double>(m_project.outPoint); }
    Q_INVOKABLE void freezeFrame();
    /// Separa o áudio dos clipes de vídeo selecionados em clipes de áudio.
    Q_INVOKABLE void detachAudio();
    /// Mede a loudness (EBU R128) dos clipes selecionados e ajusta o ganho para o alvo (LUFS).
    Q_INVOKABLE void normalizeLoudness(double targetLufs);
    /// Separa a música do clipe selecionado (IA): 0 = quatro faixas, 1 = só a voz, 2 = só o instrumental.
    Q_INVOKABLE void separateStems(int mode);
    /// Rastreamento: o clipe selecionado passa a seguir uma região do vídeo que está por baixo.
    /// Primeiro o usuário desenha a região no player (trackingPick); coordenadas do canvas 0..1.
    Q_INVOKABLE void startTrackingPick();
    Q_INVOKABLE void cancelTrackingPick();
    Q_INVOKABLE void trackRegion(double x0, double y0, double x1, double y1);
    Q_INVOKABLE void cancelTracking();
    Q_INVOKABLE void clearTracking();
    bool trackingPick() const { return m_trackingPick; }
    double trackingProgress() const { return m_trackingProgress; }

    // --- seleção e propriedades ---
    Q_INVOKABLE void select(const QString &clipId, bool additive = false);
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE void setClipProperty(const QString &clipId, const QString &key, const QVariant &value);
    /// Correção de cor avançada. Curva: canal 0 = mestre, 1..3 = R, G, B; pontos [{x, y}] (0..1).
    Q_INVOKABLE void setCurve(const QString &clipId, int channel, const QVariantList &points);
    /// HSL da faixa `band` (0 vermelho … 7 magenta); valores -1..1.
    Q_INVOKABLE void setHsl(const QString &clipId, int band, double hue, double saturation, double luminance);
    /// Roda "lift", "gamma" ou "gain": cor (x, y no disco) e luminância (-1..1).
    Q_INVOKABLE void setColorWheel(const QString &clipId, const QString &which, double x, double y, double luma);
    Q_INVOKABLE void resetGrade(const QString &clipId);
    /// Várias propriedades numa única edição (um passo de desfazer).
    Q_INVOKABLE void setClipProperties(const QString &clipId, const QVariantMap &values);
    Q_INVOKABLE void toggleKeyframe(const QString &clipId, const QString &prop);
    /// Keyframes de uma propriedade: [{t, v, ease, bx1, by1, bx2, by2}] (t relativo ao clipe).
    Q_INVOKABLE QVariantList keyframes(const QString &clipId, const QString &prop) const;
    /// Valores amostrados ao longo do clipe (para desenhar o gráfico).
    Q_INVOKABLE QVariantList sampleKeyframes(const QString &clipId, const QString &prop, int samples) const;
    Q_INVOKABLE void moveKeyframe(const QString &clipId, const QString &prop, double fromT, double toT, double value);
    Q_INVOKABLE void setKeyframeEasing(const QString &clipId, const QString &prop, double t, const QString &ease);
    Q_INVOKABLE void setKeyframeBezier(const QString &clipId, const QString &prop, double t, double x1, double y1,
                                       double x2, double y2);
    Q_INVOKABLE void setEasingForAll(const QString &clipId, const QString &prop, const QString &ease);
    Q_INVOKABLE void setTransition(const QString &clipId, const QString &kind, double durationUs);
    Q_INVOKABLE void setAnimation(const QString &clipId, bool in, const QString &kind, double durationUs);
    Q_INVOKABLE void setSpeed(const QString &clipId, double speed);
    /// Curva de velocidade [{u, v}] (lista vazia volta à velocidade constante).
    Q_INVOKABLE void setSpeedCurve(const QString &clipId, const QVariantList &points);
    Q_INVOKABLE void applySpeedPreset(const QString &clipId, const QString &presetId);
    Q_INVOKABLE void resetClipProperties(const QString &clipId, const QString &group);

    // --- histórico ---
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

    // --- reprodução ---
    Q_INVOKABLE void seek(double t);
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void stepFrames(int n);
    Q_INVOKABLE void jumpToEdge(bool end);

    // --- export ---
    Q_INVOKABLE void exportVideo(const QUrl &url, int height, double fps, const QString &codec, int crf,
                                 bool rangeOnly = false, bool hardware = false);
    /// Só o áudio mixado; o formato vem da extensão (wav, flac, m4a, opus, mp3).
    Q_INVOKABLE void exportAudio(const QUrl &url, bool rangeOnly = false);
    /// O quadro sob o cursor como imagem (PNG/JPG) na resolução do canvas.
    Q_INVOKABLE void exportFrame(const QUrl &url);
    /// Pré-renderiza o trecho de entrada/saída (ou a timeline inteira) para reprodução fluida.
    Q_INVOKABLE void renderPreview();
    Q_INVOKABLE void cancelRenderPreview();
    Q_INVOKABLE void clearRenderCache();
    /// Procura pausas na mixagem (no trecho de entrada/saída, se houver); o resultado vai para `silences`.
    Q_INVOKABLE void analyzeSilences(double thresholdDb, double minPauseMs, double paddingMs);
    /// Corta as pausas da última análise (se o projeto não mudou desde então).
    Q_INVOKABLE void removeSilences();
    Q_INVOKABLE void clearSilences();
    /// Remove trechos [{start, end}] de todas as faixas, com ripple (edição por transcrição).
    Q_INVOKABLE bool removeTimeRanges(const QVariantList &ranges, const QString &label = {});
    /// Transcreve a timeline palavra a palavra (whisper.cpp) para editar pelo texto.
    Q_INVOKABLE void generateTranscript(const QString &modelId, const QString &language);
    /// Corta da timeline o trecho das palavras indicadas (índices em `transcript`), com ripple.
    Q_INVOKABLE bool removeTranscriptWords(const QVariantList &indices);
    /// Índices das palavras de hesitação ("hum", "é…", "uh", "um"...).
    Q_INVOKABLE QVariantList fillerWordIndices() const;
    Q_INVOKABLE void clearTranscript();
    Q_INVOKABLE void cancelExport();

    // --- legendas ---
    Q_INVOKABLE void downloadModel(const QString &id);
    Q_INVOKABLE void cancelModelDownload();
    Q_INVOKABLE void generateCaptions(const QString &modelId, const QString &language, int trackIndex, bool replace);
    Q_INVOKABLE void cancelTranscription();
    Q_INVOKABLE bool importSubtitles(const QUrl &url);
    Q_INVOKABLE bool exportSubtitles(const QUrl &url);
    Q_INVOKABLE void applyStyleToTrack(const QString &clipId);

    // --- filtros e ajustes ---
    /// Aplica um filtro embutido ao clipe (vazio remove).
    Q_INVOKABLE void applyFilter(const QString &clipId, const QString &filterId);
    /// Adiciona um clipe de filtro/ajuste no cursor (afeta tudo abaixo).
    Q_INVOKABLE void addFilterClip(const QString &filterId);
    Q_INVOKABLE bool loadLut(const QString &clipId, const QUrl &url);
    /// Quadro de referência para prévias de filtro: "assetId/tempoµs" (vazio se não houver).
    Q_INVOKABLE QString previewFrameRef() const;

    // --- área de transferência e grupos ---
    Q_INVOKABLE void copySelection();
    Q_INVOKABLE void cutSelection();
    Q_INVOKABLE void paste();
    Q_INVOKABLE void copyAttributes();
    Q_INVOKABLE void pasteAttributesToSelection();
    Q_INVOKABLE void groupSelection();
    Q_INVOKABLE void ungroupSelection();
    /// Seleciona clipes que tocam [t0, t1] nas faixas dadas (seleção por retângulo).
    Q_INVOKABLE void selectInRange(double t0, double t1, const QVariantList &trackIndices, bool additive);
    Q_INVOKABLE void selectAll();

    // --- proxies ---
    Q_INVOKABLE void generateProxy(const QString &assetId);
    Q_INVOKABLE void cancelProxies();

    // --- utilitários para QML ---
    Q_INVOKABLE QString formatTime(double us) const;
    /// Atalhos: sequência portátil ("Ctrl+B") a partir de uma tecla do QML, texto nativo e comparação.
    Q_INVOKABLE QString keySequence(int key, int modifiers) const;
    Q_INVOKABLE QString shortcutText(const QString &sequence) const;
    Q_INVOKABLE bool sameShortcut(const QString &a, const QString &b) const;
    Q_INVOKABLE QString assetPath(const QString &assetId) const;

signals:
    void projectChanged();
    void assetsChanged();
    void fileChanged();
    void modifiedChanged();
    void compoundPathChanged();
    void playheadChanged();
    void playingChanged();
    void selectionChanged();
    void selectedClipChanged();
    void historyChanged();
    void revisionChanged();
    void snappingChanged();
    void exportChanged();
    void exportFinished(bool ok, const QString &message);
    void statusMessageChanged();
    void errorOccurred(const QString &message);
    void captionModelsChanged();
    void modelDownloadProgressChanged();
    void transcribeChanged();
    void proxiesChanged();
    void clipboardChanged();
    void audioJobsChanged();
    void languageChanged();
    void renderCacheChanged();
    void silencesChanged();
    void beatsChanged();
    void trackingChanged();
    void transcriptChanged();
    void scopesChanged();
    void recordingChanged();
    void recordLevelChanged();
    void scopeFrameChanged();
    void recentProjectsChanged();

private:
    bool compoundHasAudio(const clv::Sequence &seq) const;
    /** Executa uma edição com registro no histórico. `coalesceKey` agrupa edições contínuas. */
    template <typename F>
    bool edit(const QString &label, F &&fn, const QString &coalesceKey = {});
    void commitChange();
    void setProject(clv::Project p, const QString &path);
    void setStatus(const QString &msg);
    void tick();
    void refreshProxies();
    void refreshDenoise();
    void rememberRecent(const QString &path);
    void startNextDenoise();
    void refreshMattes();
    void startNextMatte();
    void refreshVoice();
    void startNextVoice();
    void refreshEffects();
    void startNextEffect();
    /// Áudio que o mixer usa antes do efeito: voz isolada, limpo ou o original.
    QString audioSourceFor(const clv::Clip &c, const clv::MediaAsset &a) const;
    void startExport(const clv::engine::ExportOptions &opts);
    void refreshRenderCache();
    void enqueueProxy(const QString &mediaPath);
    void startNextProxy();
    QString defaultTextFor(const QString &preset) const;

    clv::Project m_project;
    clv::History m_history;
    /// Níveis de fora enquanto um clipe composto está aberto (o de cima é o mais interno).
    struct CompoundFrame {
        clv::Project outer;
        clv::History history;
        int savedRevision = 0; ///< -2: salvo de dentro (vale se o conteúdo não mudou depois)
        QString clipId;
        QString seqId;
        QString name;
        clv::Micros playhead = 0;
        QStringList selection;
    };
    std::vector<CompoundFrame> m_compoundStack;
    /// O projeto inteiro, com os compostos abertos já mesclados (para salvar e exportar).
    clv::Project rootProject() const;
    QString m_filePath;
    int m_savedRevision = 0;
    int m_revisionCounter = 0;
    QStringList m_selection;
    QString m_primary; ///< último clipe clicado (inspetor e "copiar atributos" em seleções múltiplas)
    QString primaryClipId() const;
    /// Clipe cuja transição de entrada a seleção edita: ele mesmo, ou o seguinte se só encosta nele; vazio sem corte.
    QString transitionTarget(const QString &clipId) const;
    bool m_snapping = true;

    clv::Micros m_playhead = 0;
    bool m_playing = false;
    QTimer m_ticker;
    QElapsedTimer m_clock;
    clv::Micros m_playStart = 0;
    std::unique_ptr<AudioOutput> m_audio;

    QTimer m_autosave;
    std::unique_ptr<QLockFile> m_recoveryLock; ///< marca a cópia como "em uso" por esta sessão
    int m_autosavedRevision = 0;
    QString recoveryFileFor(const QString &projectId) const;
    void removeRecovery();

    QString m_lastCoalesceKey;
    QElapsedTimer m_coalesceTimer;

    mutable std::mutex m_snapMutex;
    std::shared_ptr<const clv::Project> m_snapshot;

    QNetworkAccessManager *m_network = nullptr;
    QNetworkReply *m_modelReply = nullptr;
    QString m_downloadingModel;
    double m_modelDownloadProgress = 0;
    std::atomic<bool> m_cancelTranscription{false};
    bool m_transcribing = false;
    double m_transcribeProgress = 0;

    bool m_normalizing = false;
    QStringList m_denoiseQueue;     ///< "caminho|intensidade" aguardando limpeza
    QString m_denoiseBuilding;
    double m_denoiseProgress = 0;
    std::shared_ptr<std::atomic<bool>> m_denoiseCancel = std::make_shared<std::atomic<bool>>(false);
    QQmlEngine *m_engine = nullptr;
    int m_previewResolution = 1280;
    bool m_scopesEnabled = false;
    mutable std::mutex m_scopeMutex;
    QImage m_scopeFrame;
    QVariantList m_transcript;
    int m_transcriptRevision = 0;
    QVariantList m_silences;
    int m_silenceRevision = 0;
    bool m_analyzingSilences = false;
    bool m_detectingBeats = false;
    double m_silenceProgress = 0;
    QTimer m_cacheTimer;               ///< agrupa as edições antes de recalcular as chaves do cache
    std::shared_ptr<const clv::engine::RenderCacheView> m_cacheView = std::make_shared<clv::engine::RenderCacheView>();
    QVariantList m_cachedRanges;
    bool m_rendering = false;
    double m_renderProgress = 0;
    std::shared_ptr<std::atomic<bool>> m_renderCancel = std::make_shared<std::atomic<bool>>(false); ///< para retraduzir a interface ao trocar o idioma
    std::unique_ptr<AudioRecorder> m_recorder;
    QTimer m_recordTimer;
    QString m_recordPath;
    clv::Micros m_recordStart = 0;
    double m_recordLevel = 0;
    QStringList m_effectQueue;      ///< "fonte|efeito" aguardando processamento
    QString m_effectBuilding;
    double m_effectProgress = 0;
    std::shared_ptr<std::atomic<bool>> m_effectCancel = std::make_shared<std::atomic<bool>>(false);
    QStringList m_voiceQueue;       ///< "caminho|intensidade" aguardando isolamento de voz
    QString m_voiceBuilding;
    double m_voiceProgress = 0;
    std::shared_ptr<std::atomic<bool>> m_voiceCancel = std::make_shared<std::atomic<bool>>(false);
    bool m_trackingPick = false;
    double m_trackingProgress = -1;
    std::shared_ptr<std::atomic<bool>> m_trackCancel = std::make_shared<std::atomic<bool>>(false);
    /// Vídeo logo abaixo do clipe, sob o cursor (o que ele pode seguir), ou vazio.
    QString trackingSourceFor(const QString &clipId) const;
    struct StemRequest {
        QString clipId;
        int mode = 0;
    };
    QString m_stemsBuilding; ///< mídia sendo separada em faixas
    double m_stemsProgress = 0;
    std::vector<StemRequest> m_stemRequests; ///< clipes à espera das faixas dessa mídia
    std::shared_ptr<std::atomic<bool>> m_stemsCancel = std::make_shared<std::atomic<bool>>(false);
    void placeStems(const QString &clipId, int mode);
    QStringList m_stabQueue; ///< vídeos aguardando a análise de movimento (estabilização)
    QString m_stabBuilding;
    double m_stabProgress = 0;
    std::shared_ptr<std::atomic<bool>> m_stabCancel = std::make_shared<std::atomic<bool>>(false);
    void refreshStabilize();
    void startNextStabilize();
    QStringList m_matteQueue;       ///< mídias aguardando remoção de fundo
    QString m_matteBuilding;
    double m_matteProgress = 0;
    std::shared_ptr<std::atomic<bool>> m_matteCancel = std::make_shared<std::atomic<bool>>(false);
    clv::Clipboard m_clipboard;
    std::optional<clv::Clip> m_attributeSource;
    bool m_useProxies = true;
    QStringList m_proxyQueue;       ///< caminhos de mídia aguardando proxy
    QString m_proxyBuilding;        ///< caminho em geração
    double m_proxyProgress = 0;
    std::shared_ptr<std::atomic<bool>> m_proxyCancel = std::make_shared<std::atomic<bool>>(false);

    std::atomic<bool> m_cancelExport{false};
    bool m_exporting = false;
    double m_exportProgress = 0;
    QString m_status;
};
