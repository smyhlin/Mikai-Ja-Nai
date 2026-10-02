
#include "NativeRifeControls.hpp"

#include <cmath>

#include <QtCore/QCoreApplication>
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

#include "NativeRife.hpp"
#include "NativeUserRecipes.hpp"
#include "theme/Colors.hpp"
#include "theme/Metrics.hpp"

namespace mplayer {
namespace native {

namespace theme = mikai::app::theme;

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
QString thresholdText(double value) {
    return QLocale().toString(value, 'g', QLocale::FloatingPointShortest);
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
    variant_->addItem(tr("Full"), static_cast<int>(RifeVariant::Full));
    variant_->addItem(tr("Lite"), static_cast<int>(RifeVariant::Lite));
    variant_->setCurrentIndex(variant_->findData(static_cast<int>(RifeVariant::Full)));
    form->addRow(tr("Variant"), variant_);

    ensemble_ = new QCheckBox(tr("Ensemble the variant"), this);
    ensemble_->setObjectName(QStringLiteral("nativeRifeEnsembleCheck"));
    ensemble_->setAccessibleName(tr("Ensemble the variant"));
    form->addRow(tr("Ensemble"), ensemble_);

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
    auto* thresholdValidator = new QDoubleValidator(0.0, 1.0, -1, threshold_);
    thresholdValidator->setNotation(QDoubleValidator::StandardNotation);
    thresholdValidator->setLocale(QLocale());
    threshold_->setValidator(thresholdValidator);
    threshold_->setText(thresholdText(0.150));
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
    for (int row = 0; row < form->rowCount(); ++row) {
        QLayoutItem* item = form->itemAt(row, QFormLayout::LabelRole);
        auto* label = item == nullptr ? nullptr : qobject_cast<QLabel*>(item->widget());
        if (label != nullptr) label->setWordWrap(true);
    }

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

void NativeRifeControls::updateCapabilityNote() {
    // The table is the one source of the reason; the surface only repeats it. A
    // variant this build cannot offer is named, never blank.
    const QString modelId =
        rifeModelName(RifeVersion::V4_14,
                      static_cast<RifeVariant>(variant_->currentData().toInt()),
                      ensemble_->isChecked());
    const RifeCapability* capability = findRifeCapability(modelId);
    if (capability == nullptr || capability->availability.isUsable()) {
        return;
    }
    showStatus(true, capability->availability.reason);
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
    const double sceneThreshold = QLocale().toDouble(threshold_->text(), &ok);
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
    threshold_->setText(thresholdText(stage.sceneThreshold));
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

}  // namespace native
}  // namespace mplayer
