
#include "NativeRifeControls.hpp"

#include <cmath>

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QLocale>
#include <QtCore/QSignalBlocker>
#include <QtGui/QDoubleValidator>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include <native/models/NativeRife.hpp>
#include <native/models/NativeUserRecipes.hpp>
#include "theme/Colors.hpp"
#include "theme/Metrics.hpp"

namespace mikai::app {

// The control owns the app-side surface; the recipe and type vocabulary it
// reads stays in mplayer::native and is named through this file-local
// using-directive rather than re-homed.
using namespace mplayer::native;

namespace {

/// The largest factor side the surface offers. Any positive integer is a legal
/// factor; this is the control's bound, not the contract's.
constexpr int kMaxFactorSide = 100000;

/// Keeps one combo's caption from dictating the panel's minimum width. Choice
/// captions here are whole sentences ("After spatial (upscale, then
/// interpolate)"), and a closed combo that reserved its widest caption would set
/// the whole Processing page's minimum width and push the smallest window into
/// horizontal panning. The shared choice budget bounds the widget's own hint, and
/// an ignored horizontal policy keeps that hint out of the row's minimum, so
/// neither the page's minimum nor its preferred width follows a caption. The
/// closed caption elides; the tooltip and the popup still show every choice in
/// full.
void boundCaptionWidth(QComboBox* combo) {
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo->setMinimumContentsLength(theme::kSettingsChoiceLengthChars);
    QSizePolicy policy = combo->sizePolicy();
    policy.setHorizontalPolicy(QSizePolicy::Ignored);
    combo->setSizePolicy(policy);
}

void fail(QString* reasonKey, QString* reason, const QString& key, const QString& text) {
    if (reasonKey != nullptr) *reasonKey = key;
    if (reason != nullptr) *reason = text;
}

/// Execution order is load-bearing, so equal chains compare stage by stage
/// rather than by a set comparison that would lose the order.
bool sameChain(const QVector<SpatialStage>& a, const QVector<SpatialStage>& b) {
    if (a.size() != b.size()) return false;
    for (int i = 0; i < a.size(); ++i) {
        if (a.at(i).modelId != b.at(i).modelId) return false;
        if (a.at(i).preResizeEnabled != b.at(i).preResizeEnabled) return false;
        if (a.at(i).preResizeEnabled && a.at(i).preResize != b.at(i).preResize) return false;
    }
    return true;
}

/// The editor's text for one threshold: the shortest representation of the
/// exact double in the current locale, so the text the editor shows reads back
/// as the same double.
QString thresholdText(double value, QLocale* renderedWith = nullptr) {
    const QLocale locale;
    if (renderedWith != nullptr) {
        *renderedWith = locale;
    }
    return locale.toString(value, 'g', QLocale::FloatingPointShortest);
}

}  // namespace

NativeRifeControls::NativeRifeControls(QWidget* parent) : QWidget(parent) {
    setAccessibleName(tr("RIFE interpolation"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(theme::kSpacingMedium);

    auto* form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    // A row stacks its field under its label instead of reserving the sum of both
    // widths, so a translated caption and a control can never add up to more than
    // the smallest window holds.
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setSpacing(theme::kSpacingMedium);

    variant_ = new QComboBox(this);
    variant_->setObjectName(QStringLiteral("nativeRifeVariantCombo"));
    variant_->setAccessibleName(tr("RIFE variant"));
    boundCaptionWidth(variant_);
    // The variant list is the resolver's own pin table rather than a second copy:
    // a variant this build does not pin can never be offered here, and a pinned
    // one can never be missing from it. Full precedes Lite by the table's order.
    for (const RifeModelPin& pin : rifeModelPins()) {
        if (pin.version != RifeVersion::V4_14) continue;
        if (variant_->findData(static_cast<int>(pin.variant)) >= 0) continue;
        variant_->addItem(pin.variant == RifeVariant::Lite ? tr("Lite") : tr("Full"),
                          static_cast<int>(pin.variant));
    }
    variant_->setCurrentIndex(variant_->findData(static_cast<int>(RifeVariant::Full)));
    if (variant_->currentIndex() < 0 && variant_->count() > 0) {
        variant_->setCurrentIndex(0);
    }
    form->addRow(tr("Variant"), variant_);

    // The ensemble flag. A check box caption cannot wrap, so a whole translated
    // sentence here - the Ukrainian "Застосувати ансамбль до варіанта" alone
    // measured ~473 px - reserved the panel's minimum width and pushed the page
    // past the smallest window. The interactive label is therefore the concise
    // caption, and the explanation sits in its own wrapped hint below; the caption
    // stays a real, readable phrase rather than an abbreviation, and nothing about
    // the option itself changes.
    const QString ensembleExplanation =
        tr("Applies ensembling on top of the chosen variant; it is not a third variant.");
    ensemble_ = new QCheckBox(tr("Ensemble variant"), this);
    ensemble_->setObjectName(QStringLiteral("nativeRifeEnsembleCheck"));
    ensemble_->setAccessibleName(tr("Ensemble the variant"));
    // The global stylesheet sizes fields and buttons but not a check box, whose
    // default height is under the 24 px desktop hit target. Matching the
    // settings control height keeps every actionable row measurable.
    ensemble_->setMinimumHeight(theme::kSettingsControlSize);
    ensemble_->setToolTip(ensembleExplanation);
    // The row spans the form: the check box carries its own caption, so a second
    // label column would only repeat the word and add width to the row.
    form->addRow(ensemble_);

    ensembleHint_ = new QLabel(ensembleExplanation, this);
    ensembleHint_->setObjectName(QStringLiteral("nativeRifeEnsembleHint"));
    ensembleHint_->setWordWrap(true);
    // A wrapped hint yields whatever width it is given and never votes its own
    // sentence into the panel's minimum, exactly like the status line below.
    QSizePolicy ensembleHintPolicy = ensembleHint_->sizePolicy();
    ensembleHintPolicy.setHorizontalPolicy(QSizePolicy::Ignored);
    ensembleHint_->setSizePolicy(ensembleHintPolicy);
    form->addRow(ensembleHint_);

    auto* factorRow = new QHBoxLayout();
    factorRow->setSpacing(theme::kSpacingSmall);
    factorNum_ = new QSpinBox(this);
    factorNum_->setObjectName(QStringLiteral("nativeRifeFactorNumSpin"));
    factorNum_->setAccessibleName(tr("Frame factor numerator"));
    factorNum_->setRange(1, kMaxFactorSide);
    factorNum_->setValue(2);
    factorDen_ = new QSpinBox(this);
    factorDen_->setObjectName(QStringLiteral("nativeRifeFactorDenSpin"));
    factorDen_->setAccessibleName(tr("Frame factor denominator"));
    factorDen_->setRange(1, kMaxFactorSide);
    factorDen_->setValue(1);
    auto* slash = new QLabel(QStringLiteral("/"), this);
    factorRow->addWidget(factorNum_);
    factorRow->addWidget(slash);
    factorRow->addWidget(factorDen_);
    factorRow->addStretch();
    form->addRow(tr("Frame factor"), factorRow);

    threshold_ = new QLineEdit(this);
    threshold_->setObjectName(QStringLiteral("nativeRifeSceneThresholdEdit"));
    threshold_->setAccessibleName(tr("Scene detection threshold"));
    // The validator only guides typing. The value is parsed again with the same
    // locale and validated explicitly, so nothing is rounded to a fixed number
    // of decimals and nothing is silently replaced with 0.
    thresholdValidator_ = new QDoubleValidator(0.0, 1.0, -1, threshold_);
    thresholdValidator_->setNotation(QDoubleValidator::StandardNotation);
    thresholdValidator_->setLocale(QLocale());
    threshold_->setValidator(thresholdValidator_);
    threshold_->setText(thresholdText(0.150, &thresholdLocale_));
    threshold_->setToolTip(
        tr("Above this normalized scene difference a frame pair is treated as a cut."));
    form->addRow(tr("Scene threshold"), threshold_);

    order_ = new QComboBox(this);
    order_->setObjectName(QStringLiteral("nativeRifeOrderCombo"));
    order_->setAccessibleName(tr("Temporal order"));
    boundCaptionWidth(order_);
    order_->addItem(tr("Before spatial (interpolate, then upscale)"),
                    static_cast<int>(TemporalOrder::BeforeSpatial));
    order_->addItem(tr("After spatial (upscale, then interpolate)"),
                    static_cast<int>(TemporalOrder::AfterSpatial));
    order_->setCurrentIndex(
        order_->findData(static_cast<int>(TemporalOrder::AfterSpatial)));
    form->addRow(tr("Order"), order_);

    mode_ = new QComboBox(this);
    mode_->setObjectName(QStringLiteral("nativeRifeModeCombo"));
    mode_->setAccessibleName(tr("RIFE request shape"));
    boundCaptionWidth(mode_);
    mode_->addItem(tr("RIFE only"), 0);
    mode_->addItem(tr("Combine with a stored spatial recipe"), 1);
    form->addRow(tr("Request"), mode_);

    // The closed caption is bounded, so the tooltip carries the whole choice: a
    // long order or shape stays readable without widening the row.
    for (QComboBox* combo : {order_, mode_}) {
        combo->setToolTip(combo->currentText());
        connect(combo, &QComboBox::currentIndexChanged, this,
                [combo](int) { combo->setToolTip(combo->currentText()); });
    }

    base_ = new QComboBox(this);
    base_->setObjectName(QStringLiteral("nativeRifeBaseCombo"));
    base_->setAccessibleName(tr("Spatial recipe to combine with"));
    boundCaptionWidth(base_);
    form->addRow(tr("Spatial recipe"), base_);

    // A role label is a whole translated sentence in some locales; wrapping it
    // keeps the caption readable without the caption setting the page's width.
    // Each label is also bound to the field it names, so the caption and the
    // control are one unit for assistive tech instead of two adjacent widgets.
    // A row with no label (the spanning ensemble caption and hint) has no buddy to
    // set. A row whose field is a sub-layout is still one labelled row: the frame
    // factor is a numerator and a denominator, and its label names the control the
    // fraction is read from - the leading spin box. Both spins keep their own
    // accessible names, so the fraction is not collapsed into one field.
    for (int row = 0; row < form->rowCount(); ++row) {
        QLayoutItem* labelItem = form->itemAt(row, QFormLayout::LabelRole);
        auto* label =
            labelItem == nullptr ? nullptr : qobject_cast<QLabel*>(labelItem->widget());
        if (label == nullptr) continue;
        label->setWordWrap(true);
        QLayoutItem* fieldItem = form->itemAt(row, QFormLayout::FieldRole);
        QWidget* field = fieldItem == nullptr ? nullptr : fieldItem->widget();
        if (field == nullptr && fieldItem != nullptr && fieldItem->layout() != nullptr &&
            fieldItem->layout()->count() > 0) {
            QLayoutItem* leading = fieldItem->layout()->itemAt(0);
            if (leading != nullptr) field = leading->widget();
        }
        if (field != nullptr) label->setBuddy(field);
        // Captions are collected in form order so a retranslation can restate
        // them all from the keys recorded here, rather than walking the layout
        // again and hoping it still reads the same.
        roleLabels_.append(label);
    }

    // The form's own captions, in the order the rows above added them. A
    // spanning row (the ensemble caption and its hint) carries no label, so the
    // two lists stay parallel by construction: one entry is appended per row
    // that produced a label.
    roleCaptionKeys_ = {QStringLiteral("Variant"), QStringLiteral("Frame factor"),
                        QStringLiteral("Scene threshold"), QStringLiteral("Order"),
                        QStringLiteral("Request"), QStringLiteral("Spatial recipe")};

    root->addLayout(form);

    auto* actions = new QHBoxLayout();
    actions->setSpacing(theme::kSpacingMedium);
    storeButton_ = new QPushButton(tr("Store request"), this);
    storeButton_->setObjectName(QStringLiteral("nativeRifeStoreButton"));
    storeButton_->setAccessibleName(tr("Store the RIFE request"));
    storeButton_->setMinimumHeight(theme::kSettingsControlSize);
    actions->addWidget(storeButton_);
    actions->addStretch();
    root->addLayout(actions);

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("nativeRifeStatus"));
    status_->setAccessibleName(tr("RIFE request status"));
    status_->setWordWrap(true);
    // The status can name a stored id or a path in a refusal. An unbroken token
    // has no wrap point, so a wrapped label that reserved its longest word would
    // widen the page; this one is told it may use whatever width it is given and
    // wrap the rest. The default word-wrap height-for-width stays as it is.
    QSizePolicy statusPolicy = status_->sizePolicy();
    statusPolicy.setHorizontalPolicy(QSizePolicy::Ignored);
    status_->setSizePolicy(statusPolicy);
    status_->setText(tr("Configuring a request does not run anything. The picture keeps using the "
                        "original pipeline until a prepared native pipeline exists."));
    status_->setStyleSheet(
        QStringLiteral("color: %1;").arg(QLatin1String(theme::kMuted)));
    root->addWidget(status_);

    // The exact id is one unbroken token, so it is never laid out as a label. It
    // waits behind a disclosure row and is shown verbatim in a read-only field
    // that can be selected and copied.
    idToggle_ = new QToolButton(this);
    idToggle_->setObjectName(QStringLiteral("nativeRifeIdToggle"));
    idToggle_->setText(tr("Request ID"));
    idToggle_->setCheckable(true);
    idToggle_->setArrowType(Qt::RightArrow);
    idToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    // A bare tool button is shorter than the 24 px desktop hit target; the
    // disclosure is actionable once a request is stored, so its height is set
    // to the settings control height like the other actions on this surface.
    idToggle_->setMinimumHeight(theme::kSettingsControlSize);
    idToggle_->setEnabled(false);
    auto* idRow = new QHBoxLayout();
    idRow->setSpacing(theme::kSpacingMedium);
    idRow->addWidget(idToggle_);
    idRow->addStretch();
    root->addLayout(idRow);

    idEdit_ = new QLineEdit(this);
    idEdit_->setReadOnly(true);
    idEdit_->setAccessibleName(tr("Exact request ID"));
    idEdit_->hide();
    root->addWidget(idEdit_);

    connect(idToggle_, &QToolButton::toggled, this, [this](bool open) {
        idToggle_->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        if (open) {
            idEdit_->setFocus();
            idEdit_->selectAll();
        }
        idEdit_->setVisible(open);
    });

    connect(storeButton_, &QPushButton::clicked, this, &NativeRifeControls::storeRequest);
    connect(mode_, &QComboBox::currentIndexChanged, this,
            [this](int) { updateCombineControls(); });
    connect(variant_, &QComboBox::currentIndexChanged, this,
            [this](int) { updateCapabilityNote(); });
    connect(ensemble_, &QCheckBox::toggled, this, [this](bool) { updateCapabilityNote(); });

    updateCombineControls();
    updateCapabilityNote();
}

void NativeRifeControls::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}

void NativeRifeControls::retranslateUi() {
    // In place. Every typed value the user has entered - the variant, the
    // ensemble flag, the exact numerator and denominator, the exact threshold
    // text in the current locale, the order, the shape and the chosen base - is
    // read back from its own field and never rewritten here. The stored recipes
    // are not re-read: a language switch changes words, not which base exists.

    setAccessibleName(tr("RIFE interpolation"));

    for (int i = 0; i < roleLabels_.size() && i < roleCaptionKeys_.size(); ++i) {
        roleLabels_.at(i)->setText(
            QCoreApplication::translate("NativeRifeControls",
                                        roleCaptionKeys_.at(i).toUtf8().constData()));
    }

    // The variant entries are the resolver's pin names, restated through their
    // own typed data: a reordering or a new pin can never repoint a request.
    const auto restateVariant = [this](RifeVariant value, const QString& caption) {
        const int index = variant_->findData(static_cast<int>(value));
        if (index >= 0) variant_->setItemText(index, caption);
    };
    restateVariant(RifeVariant::Full, tr("Full"));
    restateVariant(RifeVariant::Lite, tr("Lite"));
    variant_->setAccessibleName(tr("RIFE variant"));

    const QString ensembleExplanation =
        tr("Applies ensembling on top of the chosen variant; it is not a third variant.");
    ensemble_->setText(tr("Ensemble variant"));
    ensemble_->setAccessibleName(tr("Ensemble the variant"));
    ensemble_->setToolTip(ensembleExplanation);
    ensembleHint_->setText(ensembleExplanation);

    factorNum_->setAccessibleName(tr("Frame factor numerator"));
    factorDen_->setAccessibleName(tr("Frame factor denominator"));
    threshold_->setAccessibleName(tr("Scene detection threshold"));
    // Parse with the old locale before rendering with the new decimal separator.
    thresholdValidator_->setLocale(QLocale());
    bool parsed = false;
    const double sceneThreshold = thresholdLocale_.toDouble(threshold_->text(), &parsed);
    if (parsed && std::isfinite(sceneThreshold)) {
        threshold_->setText(thresholdText(sceneThreshold, &thresholdLocale_));
    }
    threshold_->setToolTip(
        tr("Above this normalized scene difference a frame pair is treated as a cut."));

    const int beforeSpatial = order_->findData(static_cast<int>(TemporalOrder::BeforeSpatial));
    const int afterSpatial = order_->findData(static_cast<int>(TemporalOrder::AfterSpatial));
    if (beforeSpatial >= 0) {
        order_->setItemText(beforeSpatial, tr("Before spatial (interpolate, then upscale)"));
    }
    if (afterSpatial >= 0) {
        order_->setItemText(afterSpatial, tr("After spatial (upscale, then interpolate)"));
    }
    order_->setAccessibleName(tr("Temporal order"));
    // The tooltip carries whatever choice is closed, so it follows the restated
    // caption rather than the one it was built with.
    order_->setToolTip(order_->currentText());

    const int rifeOnly = mode_->findData(0);
    const int combine = mode_->findData(1);
    if (rifeOnly >= 0) mode_->setItemText(rifeOnly, tr("RIFE only"));
    if (combine >= 0) {
        mode_->setItemText(combine, tr("Combine with a stored spatial recipe"));
    }
    mode_->setAccessibleName(tr("RIFE request shape"));
    mode_->setToolTip(mode_->currentText());

    // The base entries are stored recipe labels, which are data rather than UI
    // chrome: they keep whatever the store itself says. Only the empty-state
    // captions the panel writes are restated.
    base_->setAccessibleName(tr("Spatial recipe to combine with"));
    base_->setEnabled(combineSelected() && base_->count() > 0);
    base_->setToolTip(base_->count() == 0
                          ? tr("No stored spatial recipe exists to combine with.")
                          : tr("The stored spatial recipe the interpolation is combined with."));

    storeButton_->setText(tr("Store request"));
    storeButton_->setAccessibleName(tr("Store the RIFE request"));
    status_->setAccessibleName(tr("RIFE request status"));
    // The status sentence is already the resolver's verdict, recomposed by the
    // call below in the current language; its detail tooltip is chrome, so it is
    // restated here against whatever id the panel currently discloses.
    if (!statusDetailId_.isEmpty()) {
        status_->setToolTip(tr("Stored request id: %1").arg(statusDetailId_));
    }
    idToggle_->setText(tr("Request ID"));
    idEdit_->setAccessibleName(tr("Exact request ID"));

    // The availability note is the resolver's own verdict about the catalogue
    // already held in this panel: recomposing it re-reads nothing and keeps the
    // exact content id in the tooltip.
    updateCapabilityNote();
}

void NativeRifeControls::updateCapabilityNote() {
    // The resolver is the one source of fact: it joins the selected variant's
    // exact content pin to an owned temporal row that carries a proved reference.
    // The surface only repeats its verdict, so a pin the store does not hold is
    // named missing rather than assumed present, and a resolved pin is
    // *selectable* - never running. Nothing here claims Prepared or Active.
    const RifeVariant variant = static_cast<RifeVariant>(variant_->currentData().toInt());
    const bool ensemble = ensemble_->isChecked();
    const RifeModelResolution resolution =
        resolveRifeModel(RifeVersion::V4_14, variant, ensemble, catalog_);
    if (resolution.resolved()) {
        // Muted, not an error: this is availability, and the request is storable
        // either way. The exact identity rides the tooltip/description rather
        // than the label, which stays a wrap-friendly sentence.
        showStatus(true,
                   tr("The pinned original %1 is present in the owned store and usable, so the "
                      "request can be stored. It runs only once a prepared native pipeline is "
                      "activated.")
                       .arg(rifeModelName(RifeVersion::V4_14, variant, ensemble)),
                   resolution.contentId());
        return;
    }
    showStatus(true, resolution.reason);
}

void NativeRifeControls::setCatalog(const CatalogView& catalog) {
    catalog_ = catalog;
    updateCapabilityNote();
}

void NativeRifeControls::setRequestedBackend(Backend backend) { requestedBackend_ = backend; }

bool NativeRifeControls::combineSelected() const {
    return mode_->currentData().toInt() == 1;
}

void NativeRifeControls::updateCombineControls() {
    const bool combine = combineSelected();
    base_->setEnabled(combine && base_->count() > 0);
    base_->setToolTip(base_->count() == 0
                          ? tr("No stored spatial recipe exists to combine with.")
                          : tr("The stored spatial recipe the interpolation is combined with."));
}

void NativeRifeControls::refreshStoredRecipes() {
    const QString keep = base_->currentData().toString();
    {
        QSignalBlocker blocker(base_);
        base_->clear();
        const QVector<Recipe> stored = UserRecipeStore().recipes();
        for (const Recipe& recipe : stored) {
            if (recipe.shape() != RecipeShape::SpatialOnly) continue;
            base_->addItem(recipe.label.isEmpty() ? recipe.id : recipe.label, recipe.id);
        }
        const int index = base_->findData(keep);
        if (index >= 0) base_->setCurrentIndex(index);
    }
    updateCombineControls();
}

std::optional<TemporalStage> NativeRifeControls::temporalStage(QString* reasonKey,
                                                               QString* reason) const {
    // The text is parsed and validated here rather than taken on the validator's
    // word: a blank or partial editor holds no value, and a value outside [0, 1]
    // is named. Neither is coerced to 0.
    bool ok = false;
    // The editor's text is rendered in the locale that was active when it was
    // written, which is not necessarily the default locale now: a language switch
    // moves the default, and a value that predates the switch is still shown in
    // the separator it was typed with. Parsing with the wrong locale is what
    // turns a real threshold into "invalid", so the render locale is the one that
    // reads it.
    const double sceneThreshold = thresholdLocale_.toDouble(threshold_->text(), &ok);
    if (!ok || !std::isfinite(sceneThreshold) || sceneThreshold < 0.0 ||
        sceneThreshold > 1.0) {
        fail(reasonKey, reason, QStringLiteral("stage.temporal.threshold.invalid"),
             tr("The scene threshold %1 must be a finite number between 0 and 1.")
                 .arg(threshold_->text().isEmpty() ? tr("(blank)") : threshold_->text()));
        return std::nullopt;
    }

    TemporalStage stage;
    stage.version = RifeVersion::V4_14;
    stage.variant = static_cast<RifeVariant>(variant_->currentData().toInt());
    stage.ensemble = ensemble_->isChecked();
    stage.factor.num = factorNum_->value();
    stage.factor.den = factorDen_->value();
    stage.sceneThreshold = sceneThreshold;
    stage.order = static_cast<TemporalOrder>(order_->currentData().toInt());
    return stage;
}

std::optional<Recipe> NativeRifeControls::selectedBase(QString* reasonKey,
                                                       QString* reason) const {
    const QString id = base_->currentData().toString();
    if (id.isEmpty()) {
        fail(reasonKey, reason, QStringLiteral("rife.combine.noBase"),
             tr("No stored spatial recipe is selected to combine with."));
        return std::nullopt;
    }
    UserRecipeStore store;
    const std::optional<Recipe> base = store.find(id, reasonKey, reason);
    if (!base.has_value()) {
        return std::nullopt;
    }
    if (base->shape() != RecipeShape::SpatialOnly || base->spatial.isEmpty()) {
        fail(reasonKey, reason, QStringLiteral("rife.combine.notSpatial"),
             tr("The stored recipe %1 is not a spatial-only recipe, so it cannot be combined "
                "with.")
                 .arg(id));
        return std::nullopt;
    }
    return base;
}

QString NativeRifeControls::recipeLabel(const TemporalStage& stage, bool combined) const {
    QString name = tr("RIFE v4.14 · %1")
                       .arg(stage.variant == RifeVariant::Lite ? tr("Lite") : tr("Full"));
    if (stage.ensemble) {
        name += tr(" · Ensemble");
    }
    name += tr(" · %1/%2").arg(stage.factor.num).arg(stage.factor.den);
    name += stage.order == TemporalOrder::BeforeSpatial ? tr(" · before spatial")
                                                        : tr(" · after spatial");
    name += tr(" · threshold %1").arg(thresholdText(stage.sceneThreshold));
    if (combined) {
        name += tr(" · combined");
    }
    return name;
}

void NativeRifeControls::showStatus(bool ok, const QString& text, const QString& detail) {
    status_->setText(text);
    statusDetailId_ = detail;
    status_->setToolTip(detail.isEmpty() ? QString()
                                         : tr("Stored request id: %1").arg(detail));
    status_->setStyleSheet(QStringLiteral("color: %1;")
                               .arg(QLatin1String(ok ? theme::kMuted : theme::kDanger)));
    status_->show();
}

void NativeRifeControls::showRequestId(const QString& id) {
    idEdit_->setText(id);
    idToggle_->setEnabled(!id.isEmpty());
    if (id.isEmpty()) {
        idToggle_->setChecked(false);
    }
}

void NativeRifeControls::storeRequest() {
    QString key;
    QString reason;
    const std::optional<TemporalStage> stage = temporalStage(&key, &reason);
    if (!stage.has_value()) {
        showStatus(false, reason);
        return;
    }

    RifeRequest request;
    request.temporal = *stage;
    request.backend = requestedBackend_;
    if (combineSelected()) {
        std::optional<Recipe> base = selectedBase(&key, &reason);
        if (!base.has_value()) {
            showStatus(false, reason);
            return;
        }
        request.base = base;
    }
    request.label = recipeLabel(request.temporal, request.base.has_value());

    const std::optional<Recipe> recipe = buildRifeRecipe(request, &key, &reason);
    if (!recipe.has_value()) {
        showStatus(false, reason);
        return;
    }

    UserRecipeStore store;
    const std::optional<Recipe> stored = store.saveRecipe(*recipe, &key, &reason);
    if (!stored.has_value()) {
        showStatus(false, reason);
        return;
    }

    refreshStoredRecipes();
    // The visible text names the request the way the store names it to a person.
    // The exact id is one unbroken token with no wrap point: it stays in the
    // tooltip (and, through the tooltip, in the accessible description) and behind
    // the disclosure row, which carries it verbatim and lets it be copied, instead
    // of setting the panel's minimum width. Nothing about the id changes.
    const QString name = stored->label.isEmpty()
                             ? recipeLabel(request.temporal, request.base.has_value())
                             : stored->label;
    showStatus(true,
               tr("Stored the request as %1. It is selected in the recipe list; Save "
                  "persists it, and the picture keeps using the original pipeline until a "
                  "prepared native pipeline exists.")
                   .arg(name),
               stored->id);
    showRequestId(stored->id);
    emit recipeRequested(stored->id);
}

void NativeRifeControls::applyStoredRecipe(const Recipe& recipe) {
    if (!recipe.temporal.has_value()) {
        return;
    }
    const TemporalStage& stage = *recipe.temporal;

    QSignalBlocker blockVariant(variant_);
    QSignalBlocker blockEnsemble(ensemble_);
    QSignalBlocker blockNum(factorNum_);
    QSignalBlocker blockDen(factorDen_);
    QSignalBlocker blockThreshold(threshold_);
    QSignalBlocker blockOrder(order_);
    QSignalBlocker blockMode(mode_);
    QSignalBlocker blockBase(base_);

    variant_->setCurrentIndex(variant_->findData(static_cast<int>(stage.variant)));
    ensemble_->setChecked(stage.ensemble);
    factorNum_->setValue(stage.factor.num);
    factorDen_->setValue(stage.factor.den);
    threshold_->setText(thresholdText(stage.sceneThreshold, &thresholdLocale_));
    order_->setCurrentIndex(order_->findData(static_cast<int>(stage.order)));

    if (recipe.shape() == RecipeShape::Combined && !recipe.spatial.isEmpty()) {
        mode_->setCurrentIndex(mode_->findData(1));
        // The combined document does not record the base's id, so the base is the
        // stored spatial recipe whose chain is exactly this one - matched by the
        // chain, never by an index that a re-read could repoint.
        const QVector<Recipe> stored = UserRecipeStore().recipes();
        int match = -1;
        for (const Recipe& candidate : stored) {
            if (candidate.shape() != RecipeShape::SpatialOnly) continue;
            if (!sameChain(candidate.spatial, recipe.spatial)) continue;
            const int index = base_->findData(candidate.id);
            if (index >= 0) {
                match = index;
                break;
            }
        }
        base_->setCurrentIndex(match >= 0 ? match : 0);
    } else {
        mode_->setCurrentIndex(mode_->findData(0));
    }
    updateCombineControls();
    // The variant just changed under blocked signals, so the availability note
    // is refreshed explicitly: it must name the loaded variant's own store fact.
    updateCapabilityNote();
}

void NativeRifeControls::loadStoredRecipe(const QString& recipeId) {
    refreshStoredRecipes();
    if (recipeId.isEmpty()) {
        return;
    }
    UserRecipeStore store;
    QString key;
    QString reason;
    const std::optional<Recipe> recipe = store.find(recipeId, &key, &reason);
    if (!recipe.has_value()) {
        return;
    }
    showRequestId(recipe->id);
    applyStoredRecipe(*recipe);
}

}  // namespace mikai::app
