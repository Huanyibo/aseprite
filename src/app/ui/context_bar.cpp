// Aseprite
// Copyright (C) 2001-2015  David Capello
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License version 2 as
// published by the Free Software Foundation.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "app/ui/context_bar.h"

#include "app/app.h"
#include "app/commands/commands.h"
#include "app/modules/gui.h"
#include "app/modules/palettes.h"
#include "app/pref/preferences.h"
#include "app/settings/ink_type.h"
#include "app/settings/selection_mode.h"
#include "app/settings/settings.h"
#include "app/settings/settings_observers.h"
#include "app/tools/controller.h"
#include "app/tools/ink.h"
#include "app/tools/point_shape.h"
#include "app/tools/tool.h"
#include "app/tools/tool_box.h"
#include "app/ui/brush_popup.h"
#include "app/ui/button_set.h"
#include "app/ui/color_button.h"
#include "app/ui/skin/skin_theme.h"
#include "app/ui_context.h"
#include "base/bind.h"
#include "base/scoped_value.h"
#include "base/unique_ptr.h"
#include "doc/brush.h"
#include "doc/conversion_she.h"
#include "doc/image.h"
#include "doc/palette.h"
#include "she/surface.h"
#include "she/system.h"
#include "ui/button.h"
#include "ui/combobox.h"
#include "ui/int_entry.h"
#include "ui/label.h"
#include "ui/listitem.h"
#include "ui/popup_window.h"
#include "ui/preferred_size_event.h"
#include "ui/theme.h"
#include "ui/tooltips.h"

namespace app {

using namespace app::skin;
using namespace gfx;
using namespace ui;
using namespace tools;

static bool g_updatingFromTool = false;

class ContextBar::BrushTypeField : public ButtonSet
                                 , public BrushPopupDelegate {
public:
  BrushTypeField(ContextBar* owner)
    : ButtonSet(1)
    , m_owner(owner)
    , m_bitmap(BrushPopup::createSurfaceForBrush(BrushRef(nullptr)))
    , m_popupWindow(this) {
    addItem(m_bitmap);
    m_popupWindow.BrushChange.connect(&BrushTypeField::onBrushChange, this);
  }

  ~BrushTypeField() {
    closePopup();

    m_bitmap->dispose();
  }

  void updateBrush(tools::Tool* tool = nullptr) {
    if (m_bitmap)
      m_bitmap->dispose();

    m_bitmap = BrushPopup::createSurfaceForBrush(
      m_owner->activeBrush(tool));

    getItem(0)->setIcon(m_bitmap);
  }

  void setupTooltips(TooltipManager* tooltipManager) {
    m_popupWindow.setupTooltips(tooltipManager);
  }

protected:
  void onItemChange() override {
    ButtonSet::onItemChange();

    if (!m_popupWindow.isVisible())
      openPopup();
    else
      closePopup();
  }

  void onPreferredSize(PreferredSizeEvent& ev) {
    ev.setPreferredSize(Size(16, 18)*guiscale());
  }

  // BrushPopupDelegate impl
  void onDeleteBrushSlot(int slot) override {
    m_owner->removeBrush(slot);
  }

  void onDeleteAllBrushes() override {
    while (!m_owner->brushes().empty())
      m_owner->removeBrush(m_owner->brushes().size());
  }

private:
  // Returns a little rectangle that can be used by the popup as the
  // first brush position.
  gfx::Rect getPopupBox() {
    Rect rc = getBounds();
    rc.y += rc.h - 2*guiscale();
    rc.setSize(getPreferredSize());
    return rc;
  }

  void openPopup() {
    ISettings* settings = UIContext::instance()->settings();
    Tool* currentTool = settings->getCurrentTool();
    IBrushSettings* brushSettings = settings->getToolSettings(currentTool)->getBrush();
    doc::BrushRef brush = m_owner->activeBrush();

    m_popupWindow.regenerate(getPopupBox(), m_owner->brushes());
    m_popupWindow.setBrush(brush.get());

    Region rgn(m_popupWindow.getBounds().createUnion(getBounds()));
    m_popupWindow.setHotRegion(rgn);

    m_popupWindow.openWindow();
  }

  void closePopup() {
    m_popupWindow.closeWindow(NULL);
  }

  void onBrushChange(const BrushRef& brush) {
    if (brush->type() == kImageBrushType)
      m_owner->setActiveBrush(brush);
    else {
      ISettings* settings = UIContext::instance()->settings();
      Tool* currentTool = settings->getCurrentTool();
      IBrushSettings* brushSettings = settings->getToolSettings(currentTool)->getBrush();
      brushSettings->setType(brush->type());

      m_owner->setActiveBrush(
        ContextBar::createBrushFromSettings(brushSettings));
    }
  }

  ContextBar* m_owner;
  she::Surface* m_bitmap;
  BrushPopup m_popupWindow;
};

class ContextBar::BrushSizeField : public IntEntry
{
public:
  BrushSizeField() : IntEntry(Brush::kMinBrushSize, Brush::kMaxBrushSize) {
    setSuffix("px");
  }

private:
  void onValueChange() override {
    IntEntry::onValueChange();
    if (g_updatingFromTool)
      return;

    ISettings* settings = UIContext::instance()->settings();
    Tool* currentTool = settings->getCurrentTool();
    settings->getToolSettings(currentTool)
      ->getBrush()
      ->setSize(getValue());
  }
};

class ContextBar::BrushAngleField : public IntEntry
{
public:
  BrushAngleField(BrushTypeField* brushType)
    : IntEntry(0, 180)
    , m_brushType(brushType) {
    setSuffix("\xc2\xb0");
  }

protected:
  void onValueChange() override {
    IntEntry::onValueChange();
    if (g_updatingFromTool)
      return;

    ISettings* settings = UIContext::instance()->settings();
    Tool* currentTool = settings->getCurrentTool();
    settings->getToolSettings(currentTool)
      ->getBrush()
      ->setAngle(getValue());

    m_brushType->updateBrush();
  }

private:
  BrushTypeField* m_brushType;
};

class ContextBar::BrushPatternField : public ComboBox
{
public:
  BrushPatternField() : m_lock(false) {
    addItem("Pattern aligned to source");
    addItem("Pattern aligned to destination");
    addItem("Paint brush");
  }

  void setBrushPattern(BrushPattern type) {
    int index = 0;

    switch (type) {
      case BrushPattern::ALIGNED_TO_SRC: index = 0; break;
      case BrushPattern::ALIGNED_TO_DST: index = 1; break;
      case BrushPattern::PAINT_BRUSH: index = 2; break;
    }

    m_lock = true;
    setSelectedItemIndex(index);
    m_lock = false;
  }

protected:
  void onChange() override {
    ComboBox::onChange();

    if (m_lock)
      return;

    BrushPattern type = BrushPattern::ALIGNED_TO_SRC;

    switch (getSelectedItemIndex()) {
      case 0: type = BrushPattern::ALIGNED_TO_SRC; break;
      case 1: type = BrushPattern::ALIGNED_TO_DST; break;
      case 2: type = BrushPattern::PAINT_BRUSH; break;
    }

    ISettings* settings = UIContext::instance()->settings();
    Tool* currentTool = settings->getCurrentTool();
    App::instance()->preferences().brush.pattern(type);
  }

  bool m_lock;
};

class ContextBar::ToleranceField : public IntEntry
{
public:
  ToleranceField() : IntEntry(0, 255) {
  }

protected:
  void onValueChange() override {
    IntEntry::onValueChange();
    if (g_updatingFromTool)
      return;

    ISettings* settings = UIContext::instance()->settings();
    Tool* currentTool = settings->getCurrentTool();
    settings->getToolSettings(currentTool)
      ->setTolerance(getValue());
  }
};

class ContextBar::ContiguousField : public CheckBox
{
public:
  ContiguousField() : CheckBox("Contiguous") {
    setup_mini_font(this);
  }

  void setContiguous(bool state) {
    setSelected(state);
  }

protected:
  void onClick(Event& ev) override {
    CheckBox::onClick(ev);

    ISettings* settings = UIContext::instance()->settings();
    Tool* currentTool = settings->getCurrentTool();
    settings->getToolSettings(currentTool)
      ->setContiguous(isSelected());

    releaseFocus();
  }
};

class ContextBar::InkTypeField : public ComboBox
{
public:
  InkTypeField() : m_lock(false) {
    // The same order as in InkType
    addItem("Default Ink");
#if 0
    addItem("Opaque");
#endif
    addItem("Set Alpha");
    addItem("Lock Alpha");
#if 0
    addItem("Merge");
    addItem("Shading");
    addItem("Replace");
    addItem("Erase");
    addItem("Selection");
    addItem("Blur");
    addItem("Jumble");
#endif
  }

  void setInkType(InkType inkType) {
    int index = 0;

    switch (inkType) {
      case kDefaultInk: index = 0; break;
      case kSetAlphaInk: index = 1; break;
      case kLockAlphaInk: index = 2; break;
    }

    m_lock = true;
    setSelectedItemIndex(index);
    m_lock = false;
  }

protected:
  void onChange() override {
    ComboBox::onChange();

    if (m_lock)
      return;

    InkType inkType = kDefaultInk;

    switch (getSelectedItemIndex()) {
      case 0: inkType = kDefaultInk; break;
      case 1: inkType = kSetAlphaInk; break;
      case 2: inkType = kLockAlphaInk; break;
    }

    ISettings* settings = UIContext::instance()->settings();
    Tool* currentTool = settings->getCurrentTool();
    settings->getToolSettings(currentTool)->setInkType(inkType);
  }

  void onCloseListBox() override {
    releaseFocus();
  }

  bool m_lock;
};

class ContextBar::InkOpacityField : public IntEntry
{
public:
  InkOpacityField() : IntEntry(0, 255) {
  }

protected:
  void onValueChange() override {
    IntEntry::onValueChange();
    if (g_updatingFromTool)
      return;

    ISettings* settings = UIContext::instance()->settings();
    Tool* currentTool = settings->getCurrentTool();
    settings->getToolSettings(currentTool)
      ->setOpacity(getValue());
  }
};

class ContextBar::SprayWidthField : public IntEntry
{
public:
  SprayWidthField() : IntEntry(1, 32) {
  }

protected:
  void onValueChange() override {
    IntEntry::onValueChange();
    if (g_updatingFromTool)
      return;

    ISettings* settings = UIContext::instance()->settings();
    Tool* currentTool = settings->getCurrentTool();
    settings->getToolSettings(currentTool)
      ->setSprayWidth(getValue());
  }
};

class ContextBar::SpraySpeedField : public IntEntry
{
public:
  SpraySpeedField() : IntEntry(1, 100) {
  }

protected:
  void onValueChange() override {
    IntEntry::onValueChange();
    if (g_updatingFromTool)
      return;

    ISettings* settings = UIContext::instance()->settings();
    Tool* currentTool = settings->getCurrentTool();
    settings->getToolSettings(currentTool)
      ->setSpraySpeed(getValue());
  }
};


class ContextBar::TransparentColorField : public ColorButton
{
public:
  TransparentColorField() : ColorButton(app::Color::fromMask(), IMAGE_RGB) {
    Change.connect(Bind<void>(&TransparentColorField::onChange, this));
  }

protected:
  void onChange() {
    UIContext::instance()->settings()->selection()->setMoveTransparentColor(getColor());
  }
};

class ContextBar::RotAlgorithmField : public ComboBox
{
public:
  RotAlgorithmField() {
    // We use "m_lockChange" variable to avoid setting the rotation
    // algorithm when we call ComboBox::addItem() (because the first
    // addItem() generates an onChange() event).
    m_lockChange = true;
    addItem(new Item("Fast Rotation", kFastRotationAlgorithm));
    addItem(new Item("RotSprite", kRotSpriteRotationAlgorithm));
    m_lockChange = false;

    setSelectedItemIndex((int)UIContext::instance()->settings()
      ->selection()->getRotationAlgorithm());
  }

protected:
  void onChange() override {
    if (m_lockChange)
      return;

    UIContext::instance()->settings()->selection()
      ->setRotationAlgorithm(static_cast<Item*>(getSelectedItem())->algo());
  }

  void onCloseListBox() override {
    releaseFocus();
  }

private:
  class Item : public ListItem {
  public:
    Item(const std::string& text, RotationAlgorithm algo) :
      ListItem(text),
      m_algo(algo) {
    }

    RotationAlgorithm algo() const { return m_algo; }

  private:
    RotationAlgorithm m_algo;
  };

  bool m_lockChange;
};

#if 0 // TODO for v1.1 to avoid changing the UI

class ContextBar::FreehandAlgorithmField : public Button
                                         , public IButtonIcon
{
public:
  FreehandAlgorithmField()
    : Button("")
    , m_popupWindow(NULL)
    , m_tooltipManager(NULL) {
    setup_mini_look(this);
    setIconInterface(this);
  }

  ~FreehandAlgorithmField() {
    closePopup();
    setIconInterface(NULL);
  }

  void setupTooltips(TooltipManager* tooltipManager) {
    m_tooltipManager = tooltipManager;
  }

  void setFreehandAlgorithm(FreehandAlgorithm algo) {
    int part = PART_FREEHAND_ALGO_DEFAULT;
    m_freehandAlgo = algo;
    switch (m_freehandAlgo) {
      case kDefaultFreehandAlgorithm:
        part = PART_FREEHAND_ALGO_DEFAULT;
        break;
      case kPixelPerfectFreehandAlgorithm:
        part = PART_FREEHAND_ALGO_PIXEL_PERFECT;
        break;
      case kDotsFreehandAlgorithm:
        part = PART_FREEHAND_ALGO_DOTS;
        break;
    }
    m_bitmap = static_cast<SkinTheme*>(getTheme())->get_part(part);
    invalidate();
  }

  // IButtonIcon implementation
  void destroy() override {
    // Do nothing, BrushTypeField is added as a widget in the
    // ContextBar, so it will be destroyed together with the
    // ContextBar.
  }

  int getWidth() override {
    return m_bitmap->width();
  }

  int getHeight() override {
    return m_bitmap->height();
  }

  she::Surface* getNormalIcon() override {
    return m_bitmap;
  }

  she::Surface* getSelectedIcon() override {
    return m_bitmap;
  }

  she::Surface* getDisabledIcon() override {
    return m_bitmap;
  }

  int getIconAlign() override {
    return JI_CENTER | JI_MIDDLE;
  }

protected:
  void onClick(Event& ev) override {
    Button::onClick(ev);

    if (!m_popupWindow || !m_popupWindow->isVisible())
      openPopup();
    else
      closePopup();
  }

  void onPreferredSize(PreferredSizeEvent& ev) {
    ev.setPreferredSize(Size(16, 18)*guiscale());
  }

private:
  void openPopup() {
    SkinTheme* theme = static_cast<SkinTheme*>(getTheme());

    Border border = Border(2, 2, 2, 3)*guiscale();
    Rect rc = getBounds();
    rc.y += rc.h;
    rc.w *= 3;
    m_popupWindow = new PopupWindow("", PopupWindow::kCloseOnClickInOtherWindow);
    m_popupWindow->setAutoRemap(false);
    m_popupWindow->setBorder(border);
    m_popupWindow->setBounds(rc + border);

    Region rgn(m_popupWindow->getBounds().createUnion(getBounds()));
    m_popupWindow->setHotRegion(rgn);
    m_freehandAlgoButton = new ButtonSet(3);
    m_freehandAlgoButton->addItem(theme->get_part(PART_FREEHAND_ALGO_DEFAULT));
    m_freehandAlgoButton->addItem(theme->get_part(PART_FREEHAND_ALGO_PIXEL_PERFECT));
    m_freehandAlgoButton->addItem(theme->get_part(PART_FREEHAND_ALGO_DOTS));
    m_freehandAlgoButton->setSelectedItem((int)m_freehandAlgo);
    m_freehandAlgoButton->ItemChange.connect(&FreehandAlgorithmField::onFreehandAlgoChange, this);
    m_freehandAlgoButton->setTransparent(true);
    m_freehandAlgoButton->setBgColor(gfx::ColorNone);

    m_tooltipManager->addTooltipFor(at(0), "Normal trace", JI_TOP);
    m_tooltipManager->addTooltipFor(at(1), "Pixel-perfect trace", JI_TOP);
    m_tooltipManager->addTooltipFor(at(2), "Dots", JI_TOP);

    m_popupWindow->addChild(m_freehandAlgoButton);
    m_popupWindow->openWindow();
  }

  void closePopup() {
    if (m_popupWindow) {
      m_popupWindow->closeWindow(NULL);
      delete m_popupWindow;
      m_popupWindow = NULL;
      m_freehandAlgoButton = NULL;
    }
  }

  void onFreehandAlgoChange() {
    setFreehandAlgorithm(
      (FreehandAlgorithm)m_freehandAlgoButton->getSelectedItem());

    ISettings* settings = UIContext::instance()->settings();
    Tool* currentTool = settings->getCurrentTool();
    settings->getToolSettings(currentTool)
      ->setFreehandAlgorithm(m_freehandAlgo);
  }

  she::Surface* m_bitmap;
  FreehandAlgorithm m_freehandAlgo;
  PopupWindow* m_popupWindow;
  ButtonSet* m_freehandAlgoButton;
  TooltipManager* m_tooltipManager;
};

#else

class ContextBar::FreehandAlgorithmField : public CheckBox
{
public:
  FreehandAlgorithmField() : CheckBox("Pixel-perfect") {
    setup_mini_font(this);
  }

  void setupTooltips(TooltipManager* tooltipManager) {
    // Do nothing
  }

  void setFreehandAlgorithm(FreehandAlgorithm algo) {
    switch (algo) {
      case kDefaultFreehandAlgorithm:
        setSelected(false);
        break;
      case kPixelPerfectFreehandAlgorithm:
        setSelected(true);
        break;
      case kDotsFreehandAlgorithm:
        // Not available
        break;
    }
  }

protected:

  void onClick(Event& ev) override {
    CheckBox::onClick(ev);

    ISettings* settings = UIContext::instance()->settings();
    Tool* currentTool = settings->getCurrentTool();
    settings->getToolSettings(currentTool)
      ->setFreehandAlgorithm(isSelected() ?
        kPixelPerfectFreehandAlgorithm:
        kDefaultFreehandAlgorithm);

    releaseFocus();
  }
};

#endif

class ContextBar::SelectionModeField : public ButtonSet
{
public:
  SelectionModeField() : ButtonSet(3) {
    SkinTheme* theme = static_cast<SkinTheme*>(getTheme());

    addItem(theme->get_part(PART_SELECTION_REPLACE));
    addItem(theme->get_part(PART_SELECTION_ADD));
    addItem(theme->get_part(PART_SELECTION_SUBTRACT));

    setSelectedItem(
      (int)UIContext::instance()->settings()
      ->selection()->getSelectionMode());
  }

  void setupTooltips(TooltipManager* tooltipManager) {
    tooltipManager->addTooltipFor(at(0), "Replace selection", JI_BOTTOM);
    tooltipManager->addTooltipFor(at(1), "Add to selection\n(Shift)", JI_BOTTOM);
    tooltipManager->addTooltipFor(at(2), "Subtract from selection\n(Shift+Alt)", JI_BOTTOM);
  }

  void setSelectionMode(SelectionMode mode) {
    setSelectedItem((int)mode);
    invalidate();
  }

protected:
  void onItemChange() override {
    ButtonSet::onItemChange();

    UIContext::instance()->settings()->selection()
      ->setSelectionMode((SelectionMode)selectedItem());
  }
};

class ContextBar::DropPixelsField : public ButtonSet
{
public:
  DropPixelsField() : ButtonSet(2) {
    SkinTheme* theme = static_cast<SkinTheme*>(getTheme());

    addItem(theme->get_part(PART_DROP_PIXELS_OK));
    addItem(theme->get_part(PART_DROP_PIXELS_CANCEL));
    setOfferCapture(false);
  }

  void setupTooltips(TooltipManager* tooltipManager) {
    tooltipManager->addTooltipFor(at(0), "Drop pixels here", JI_BOTTOM);
    tooltipManager->addTooltipFor(at(1), "Cancel drag and drop", JI_BOTTOM);
  }

  Signal1<void, ContextBarObserver::DropAction> DropPixels;

protected:
  void onItemChange() override {
    ButtonSet::onItemChange();

    switch (selectedItem()) {
      case 0: DropPixels(ContextBarObserver::DropPixels); break;
      case 1: DropPixels(ContextBarObserver::CancelDrag); break;
    }
  }
};

class ContextBar::GrabAlphaField : public CheckBox
{
public:
  GrabAlphaField() : CheckBox("Grab Alpha") {
    setup_mini_font(this);
  }

protected:
  void onClick(Event& ev) override {
    CheckBox::onClick(ev);

    UIContext::instance()->settings()->setGrabAlpha(isSelected());

    releaseFocus();
  }
};

class ContextBar::AutoSelectLayerField : public CheckBox
{
public:
  AutoSelectLayerField() : CheckBox("Auto Select Layer") {
    setup_mini_font(this);
  }

protected:
  void onClick(Event& ev) override {
    CheckBox::onClick(ev);

    UIContext::instance()->settings()->setAutoSelectLayer(isSelected());

    releaseFocus();
  }
};

ContextBar::ContextBar()
  : Box(JI_HORIZONTAL)
  , m_toolSettings(NULL)
{
  border_width.b = 2*guiscale();

  SkinTheme* theme = static_cast<SkinTheme*>(getTheme());
  setBgColor(theme->colors.workspace());

  addChild(m_selectionOptionsBox = new HBox());
  m_selectionOptionsBox->addChild(m_dropPixels = new DropPixelsField());
  m_selectionOptionsBox->addChild(m_selectionMode = new SelectionModeField);
  m_selectionOptionsBox->addChild(m_transparentColor = new TransparentColorField);
  m_selectionOptionsBox->addChild(m_rotAlgo = new RotAlgorithmField());

  addChild(m_brushType = new BrushTypeField(this));
  addChild(m_brushSize = new BrushSizeField());
  addChild(m_brushAngle = new BrushAngleField(m_brushType));
  addChild(m_brushPatternField = new BrushPatternField());

  addChild(m_toleranceLabel = new Label("Tolerance:"));
  addChild(m_tolerance = new ToleranceField());
  addChild(m_contiguous = new ContiguousField());

  addChild(m_inkType = new InkTypeField());

  addChild(m_opacityLabel = new Label("Opacity:"));
  addChild(m_inkOpacity = new InkOpacityField());

  addChild(m_grabAlpha = new GrabAlphaField());

  addChild(m_autoSelectLayer = new AutoSelectLayerField());

  // addChild(new InkChannelTargetField());
  // addChild(new InkShadeField());
  // addChild(new InkSelectionField());

  addChild(m_sprayBox = new HBox());
  m_sprayBox->addChild(setup_mini_font(new Label("Spray:")));
  m_sprayBox->addChild(m_sprayWidth = new SprayWidthField());
  m_sprayBox->addChild(m_spraySpeed = new SpraySpeedField());

  addChild(m_freehandBox = new HBox());
#if 0                           // TODO for v1.1
  Label* freehandLabel;
  m_freehandBox->addChild(freehandLabel = new Label("Freehand:"));
  setup_mini_font(freehandLabel);
#endif
  m_freehandBox->addChild(m_freehandAlgo = new FreehandAlgorithmField());

  setup_mini_font(m_toleranceLabel);
  setup_mini_font(m_opacityLabel);

  TooltipManager* tooltipManager = new TooltipManager();
  addChild(tooltipManager);

  tooltipManager->addTooltipFor(m_brushType, "Brush Type", JI_BOTTOM);
  tooltipManager->addTooltipFor(m_brushSize, "Brush Size (in pixels)", JI_BOTTOM);
  tooltipManager->addTooltipFor(m_brushAngle, "Brush Angle (in degrees)", JI_BOTTOM);
  tooltipManager->addTooltipFor(m_inkOpacity, "Opacity (Alpha value in RGBA)", JI_BOTTOM);
  tooltipManager->addTooltipFor(m_sprayWidth, "Spray Width", JI_BOTTOM);
  tooltipManager->addTooltipFor(m_spraySpeed, "Spray Speed", JI_BOTTOM);
  tooltipManager->addTooltipFor(m_transparentColor, "Transparent Color", JI_BOTTOM);
  tooltipManager->addTooltipFor(m_rotAlgo, "Rotation Algorithm", JI_BOTTOM);
  tooltipManager->addTooltipFor(m_freehandAlgo, "Freehand trace algorithm", JI_BOTTOM);
  tooltipManager->addTooltipFor(m_grabAlpha,
    "When checked the tool picks the color from the active layer, and its alpha\n"
    "component is used to setup the opacity level of all drawing tools.\n\n"
    "When unchecked -the default behavior- the color is picked\n"
    "from the composition of all sprite layers.", JI_LEFT | JI_TOP);

  m_brushType->setupTooltips(tooltipManager);
  m_selectionMode->setupTooltips(tooltipManager);
  m_dropPixels->setupTooltips(tooltipManager);
  m_freehandAlgo->setupTooltips(tooltipManager);

  App::instance()->BrushSizeAfterChange.connect(&ContextBar::onBrushSizeChange, this);
  App::instance()->BrushAngleAfterChange.connect(&ContextBar::onBrushAngleChange, this);
  App::instance()->CurrentToolChange.connect(&ContextBar::onCurrentToolChange, this);
  m_dropPixels->DropPixels.connect(&ContextBar::onDropPixels, this);

  setActiveBrush(createBrushFromSettings());
}

ContextBar::~ContextBar()
{
  if (m_toolSettings)
    m_toolSettings->removeObserver(this);
}

bool ContextBar::onProcessMessage(Message* msg)
{
  return Box::onProcessMessage(msg);
}

void ContextBar::onPreferredSize(PreferredSizeEvent& ev)
{
  ev.setPreferredSize(gfx::Size(0, 18*guiscale())); // TODO calculate height
}

void ContextBar::onSetOpacity(int newOpacity)
{
  m_inkOpacity->setTextf("%d", newOpacity);
}

void ContextBar::onBrushSizeChange()
{
  if (m_activeBrush->type() != kImageBrushType)
    discardActiveBrush();
}

void ContextBar::onBrushAngleChange()
{
  if (m_activeBrush->type() != kImageBrushType)
    discardActiveBrush();
}

void ContextBar::onCurrentToolChange()
{
  if (m_activeBrush->type() != kImageBrushType)
    setActiveBrush(ContextBar::createBrushFromSettings());
  else {
    ISettings* settings = UIContext::instance()->settings();
    updateFromTool(settings->getCurrentTool());
  }
}

void ContextBar::onDropPixels(ContextBarObserver::DropAction action)
{
  notifyObservers(&ContextBarObserver::onDropPixels, action);
}

void ContextBar::updateFromTool(tools::Tool* tool)
{
  base::ScopedValue<bool> lockFlag(g_updatingFromTool, true, false);

  ISettings* settings = UIContext::instance()->settings();
  IToolSettings* toolSettings = settings->getToolSettings(tool);
  IBrushSettings* brushSettings = toolSettings->getBrush();

  if (m_toolSettings)
    m_toolSettings->removeObserver(this);
  m_toolSettings = toolSettings;
  m_toolSettings->addObserver(this);

  m_brushType->updateBrush(tool);
  m_brushSize->setTextf("%d", brushSettings->getSize());
  m_brushAngle->setTextf("%d", brushSettings->getAngle());
  m_brushPatternField->setBrushPattern(
    App::instance()->preferences().brush.pattern());

  m_tolerance->setTextf("%d", toolSettings->getTolerance());
  m_contiguous->setSelected(toolSettings->getContiguous());

  m_inkType->setInkType(toolSettings->getInkType());
  m_inkOpacity->setTextf("%d", toolSettings->getOpacity());

  m_grabAlpha->setSelected(settings->getGrabAlpha());
  m_autoSelectLayer->setSelected(settings->getAutoSelectLayer());
  m_freehandAlgo->setFreehandAlgorithm(toolSettings->getFreehandAlgorithm());

  m_sprayWidth->setValue(toolSettings->getSprayWidth());
  m_spraySpeed->setValue(toolSettings->getSpraySpeed());

  // True if the current tool needs opacity options
  bool hasOpacity = (tool->getInk(0)->isPaint() ||
                     tool->getInk(0)->isEffect() ||
                     tool->getInk(1)->isPaint() ||
                     tool->getInk(1)->isEffect());

  // True if we have an image as brush
  bool hasImageBrush = (activeBrush()->type() == kImageBrushType);

  // True if the current tool is eyedropper.
  bool isEyedropper =
    (tool->getInk(0)->isEyedropper() ||
     tool->getInk(1)->isEyedropper());

  // True if the current tool is move tool.
  bool isMove =
    (tool->getInk(0)->isCelMovement() ||
     tool->getInk(1)->isCelMovement());

  // True if it makes sense to change the ink property for the current
  // tool.
  bool hasInk = hasOpacity;

  // True if the current tool needs tolerance options
  bool hasTolerance = (tool->getPointShape(0)->isFloodFill() ||
                       tool->getPointShape(1)->isFloodFill());

  // True if the current tool needs spray options
  bool hasSprayOptions = (tool->getPointShape(0)->isSpray() ||
                          tool->getPointShape(1)->isSpray());

  bool hasSelectOptions = (tool->getInk(0)->isSelection() ||
                           tool->getInk(1)->isSelection());

  bool isFreehand =
    (tool->getController(0)->isFreehand() ||
     tool->getController(1)->isFreehand());

  // Show/Hide fields
  m_brushType->setVisible(hasOpacity);
  m_brushSize->setVisible(hasOpacity && !hasImageBrush);
  m_brushAngle->setVisible(hasOpacity && !hasImageBrush);
  m_brushPatternField->setVisible(hasOpacity && hasImageBrush);
  m_opacityLabel->setVisible(hasOpacity);
  m_inkType->setVisible(hasInk && !hasImageBrush);
  m_inkOpacity->setVisible(hasOpacity);
  m_grabAlpha->setVisible(isEyedropper);
  m_autoSelectLayer->setVisible(isMove);
  m_freehandBox->setVisible(isFreehand && hasOpacity);
  m_toleranceLabel->setVisible(hasTolerance);
  m_tolerance->setVisible(hasTolerance);
  m_contiguous->setVisible(hasTolerance);
  m_sprayBox->setVisible(hasSprayOptions);
  m_selectionOptionsBox->setVisible(hasSelectOptions);
  m_selectionMode->setVisible(true);
  m_dropPixels->setVisible(false);

  layout();
}

void ContextBar::updateForMovingPixels()
{
  tools::Tool* tool = App::instance()->getToolBox()->getToolById(
    tools::WellKnownTools::RectangularMarquee);
  if (tool)
    updateFromTool(tool);

  m_dropPixels->deselectItems();
  m_dropPixels->setVisible(true);
  m_selectionMode->setVisible(false);
  layout();
}

void ContextBar::updateSelectionMode(SelectionMode mode)
{
  if (!m_selectionMode->isVisible())
    return;

  m_selectionMode->setSelectionMode(mode);
}

void ContextBar::updateAutoSelectLayer(bool state)
{
  if (!m_autoSelectLayer->isVisible())
    return;

  m_autoSelectLayer->setSelected(state);
}

int ContextBar::addBrush(const doc::BrushRef& brush)
{
  // Use an empty slot
  for (size_t i=0; i<m_brushes.size(); ++i) {
    if (!m_brushes[i]) {
      m_brushes[i] = brush;
      return i+1;
    }
  }

  m_brushes.push_back(brush);
  return (int)m_brushes.size(); // Returns the slot
}

void ContextBar::removeBrush(int slot)
{
  --slot;
  if (slot >= 0 && slot < (int)m_brushes.size()) {
    m_brushes[slot].reset();

    // Erase empty trailing slots
    while (!m_brushes.empty() &&
           !m_brushes[m_brushes.size()-1])
      m_brushes.erase(--m_brushes.end());
  }
}

void ContextBar::setActiveBrushBySlot(int slot)
{
  --slot;
  if (slot >= 0 && slot < (int)m_brushes.size() &&
      m_brushes[slot]) {
    setActiveBrush(m_brushes[slot]);
  }
}

void ContextBar::setActiveBrush(const doc::BrushRef& brush)
{
  m_activeBrush = brush;

  ISettings* settings = UIContext::instance()->settings();
  updateFromTool(settings->getCurrentTool());
}

doc::BrushRef ContextBar::activeBrush(tools::Tool* tool) const
{
  if (!tool ||
      (tool->getInk(0)->isPaint() &&
       m_activeBrush->type() == kImageBrushType)) {
    m_activeBrush->setPattern(App::instance()->preferences().brush.pattern());
    return m_activeBrush;
  }

  ISettings* settings = UIContext::instance()->settings();
  IToolSettings* toolSettings = settings->getToolSettings(tool);
  return ContextBar::createBrushFromSettings(toolSettings->getBrush());
}

void ContextBar::discardActiveBrush()
{
  setActiveBrush(ContextBar::createBrushFromSettings());
}

// static
doc::BrushRef ContextBar::createBrushFromSettings(IBrushSettings* brushSettings)
{
  if (brushSettings == nullptr) {
    ISettings* settings = UIContext::instance()->settings();
    tools::Tool* tool = settings->getCurrentTool();
    IToolSettings* toolSettings = settings->getToolSettings(tool);
    brushSettings = toolSettings->getBrush();
  }

  doc::BrushRef brush;
  brush.reset(
    new Brush(
      brushSettings->getType(),
      brushSettings->getSize(),
      brushSettings->getAngle()));
  return brush;
}

} // namespace app
