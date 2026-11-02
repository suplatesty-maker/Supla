#pragma once

// ============================================================================
// velux_web.h - strona konfiguracji VELUX w panelu WWW SUPLA (tryb config)
// ============================================================================
// Pokazuje stan radia, ostatnie zdarzenie (TX/RX) i po jednym okienku na
// każdy produkt: zasilanie odbiornika (preambuła), profil klas nauki 0x30
// oraz akcję (nauka / wypisanie / nowa tożsamość).
//
// Akcje trafiają do kolejki radia (queuePending) i wykonują się w pętli
// głównej - nie w tasku serwera WWW.
// ============================================================================

#include <supla/network/html_element.h>
#include <supla/network/web_sender.h>

#include "velux_radio.h"

class VeluxHtml : public Supla::HtmlElement {
 public:
  explicit VeluxHtml(velux::VeluxRadio *radio);

  void send(Supla::WebSender *sender) override;
  bool handleResponse(const char *key, const char *value) override;

 private:
  velux::VeluxRadio *radio_ = nullptr;
};
