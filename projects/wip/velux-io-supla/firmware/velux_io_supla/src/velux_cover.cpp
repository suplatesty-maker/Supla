/// @file velux_cover.cpp
/// @brief Kanał SUPLA sterowany komendami 1W io-homecontrol.

#include "velux_cover.h"

#include <supla/log_wrapper.h>
#include <supla/storage/storage.h>

#include <supla-common/proto.h>

namespace {

/// Ten sam limit, co w RollerShutterInterface (RS_MAX_OPERATION_TIME_MS).
constexpr uint32_t RS_MAX_TIME_MS = 600000;

}  // namespace

VeluxCover::VeluxCover(velux::VeluxRadio *radio, uint8_t slot, const velux::SlotConfig &config)
    : RollerShutterInterface(false), radio_(radio), slot_(slot), product_name_(config.name) {
  // Czasy przejazdu są tylko po to, żeby aplikacja SUPLA animowała pozycję;
  // nie da się ich zmierzyć przez radio (1W nie raportuje).
  openingTimeMs = config.opening_time_ms;
  closingTimeMs = config.closing_time_ms;

  switch (config.kind) {
    case velux::ProductKind::WINDOW_OPENER:
      channel.setDefaultFunction(SUPLA_CHANNELFNC_CONTROLLINGTHEROOFWINDOW);
      break;
    case velux::ProductKind::AWNING:
      channel.setDefaultFunction(SUPLA_CHANNELFNC_TERRACE_AWNING);
      break;
    case velux::ProductKind::ROLLER_SHUTTER:
    default:
      break;
  }
  channel.setFlag(SUPLA_CHANNEL_FLAG_RS_SBS_AND_STOP_ACTIONS);
}

void VeluxCover::onLoadState() {
  RollerShutterInterface::onLoadState();

  // 1W nie zna pozycji produktu. Po pierwszym starcie nie ma czego przywrócić,
  // a aplikacja bez znanej pozycji nie pokazuje suwaka - przyjmujemy więc
  // "zamknięte" (100%). To tylko szacunek: pierwszą komendą Otwórz/Zamknij
  // użytkownik go skoryguje.
  if (getCurrentPosition() < 0) {
    setCurrentPosition(100);
    SUPLA_LOG_INFO("VELUX[%d] brak zapisanej pozycji - zakładam 100%% (zamknięte)", channel.getChannelNumber());
  }
}

void VeluxCover::iterateAlways() {
  if (pending_stop_) {
    pending_stop_ = false;
    sendStop_();
  }
  if (newTargetPositionAvailable) {
    newTargetPositionAvailable = false;
    applyTarget_();
  }
  updateAnimation_();

  // Wysyła bieżącą pozycję do serwera (z własnym ograniczeniem do 300 ms).
  RollerShutterInterface::iterateAlways();
}

int32_t VeluxCover::handleNewValueFromServer(TSD_SuplaChannelNewValue *newValue) {
  if (newValue == nullptr) {
    return -1;
  }
  const int8_t task = static_cast<int8_t>(newValue->value[0]);
  SUPLA_LOG_INFO("VELUX[%d] wartość z serwera: %d", channel.getChannelNumber(), task);

  // Wartości z SUPLA: 0 = stop, 1 = dół, 2 = góra, 3 = dół/stop, 4 = góra/stop,
  // 5 = krok po kroku, 10..110 = pozycja 0..100%. Bit tiltu (value[1])
  // ignorujemy - 1W go nie obsługuje.
  switch (task) {
    case 0:
      stop();
      break;
    case 1:
      moveDown();
      break;
    case 2:
      moveUp();
      break;
    case 3:
      if (inMove()) {
        stop();
      } else {
        moveDown();
      }
      break;
    case 4:
      if (inMove()) {
        stop();
      } else {
        moveUp();
      }
      break;
    case 5:  // step-by-step
      if (inMove()) {
        stop();
      } else if (lastDirectionWasOpen()) {
        moveDown();
      } else if (lastDirectionWasClose()) {
        moveUp();
      } else if (getCurrentPosition() < 50) {
        moveDown();
      } else {
        moveUp();
      }
      break;
    default:
      if (task >= 10 && task <= 110) {
        setTargetPosition(task - 10);
      } else {
        SUPLA_LOG_DEBUG("VELUX[%d] ignoruję wartość %d", channel.getChannelNumber(), task);
      }
      break;
  }
  return -1;
}

Supla::ApplyConfigResult VeluxCover::applyChannelConfig(TSD_ChannelConfig *result, bool local) {
  (void)(local);
  if (result == nullptr) {
    return Supla::ApplyConfigResult::DataError;
  }
  if (result->ConfigSize == 0) {
    return Supla::ApplyConfigResult::SetChannelConfigNeeded;
  }
  if (result->ConfigType != SUPLA_CONFIG_TYPE_DEFAULT) {
    return Supla::ApplyConfigResult::NotSupported;
  }
  if (result->ConfigSize != sizeof(TChannelConfig_RollerShutter)) {
    return Supla::ApplyConfigResult::DataError;
  }

  auto *config = reinterpret_cast<TChannelConfig_RollerShutter *>(result->Config);
  bool changed = false;
  if (config->OpeningTimeMS > 0) {
    uint32_t value = static_cast<uint32_t>(config->OpeningTimeMS);
    if (value > RS_MAX_TIME_MS) {
      value = RS_MAX_TIME_MS;
    }
    if (value != openingTimeMs) {
      openingTimeMs = value;
      changed = true;
    }
  }
  if (config->ClosingTimeMS > 0) {
    uint32_t value = static_cast<uint32_t>(config->ClosingTimeMS);
    if (value > RS_MAX_TIME_MS) {
      value = RS_MAX_TIME_MS;
    }
    if (value != closingTimeMs) {
      closingTimeMs = value;
      changed = true;
    }
  }
  if (changed) {
    SUPLA_LOG_INFO("VELUX[%d] czasy przejazdu z serwera: otwieranie %u ms, zamykanie %u ms",
                   channel.getChannelNumber(),
                   static_cast<unsigned>(openingTimeMs),
                   static_cast<unsigned>(closingTimeMs));
    Supla::Storage::ScheduleSave(2000);
  }
  // Świadomie nie wołamy bazowej implementacji: ona przy każdej zmianie
  // czasów oznacza kanał jako "nie skalibrowany" i kasuje znaną pozycję.
  return Supla::ApplyConfigResult::Success;
}

void VeluxCover::stop() {
  SUPLA_LOG_INFO("VELUX[%d] STOP (użytkownik)", channel.getChannelNumber());
  moving_ = false;
  move_duration_ms_ = 0;
  currentDirection = Supla::Control::Directions::STOP_DIR;
  targetPosition = STOP_POSITION;
  newTargetPositionAvailable = false;
  pending_stop_ = true;
}

void VeluxCover::applyTarget_() {
  const int8_t target = targetPosition;
  uint8_t position = 0;
  if (target == STOP_POSITION || target == UNKNOWN_POSITION) {
    // STOP obsługuje stop(); wartość UNKNOWN_POSITION oznacza u nas tylko
    // komendę samego tiltu, której 1W nie ma.
    return;
  } else if (target == MOVE_UP_POSITION) {
    position = 0;
  } else if (target == MOVE_DOWN_POSITION) {
    position = 100;
  } else if (target >= 0 && target <= 100) {
    position = static_cast<uint8_t>(target);
  } else {
    return;
  }

  targetPosition = static_cast<int8_t>(position);
  const bool two_way = radio_ != nullptr && radio_->slotTwoWayReady(slot_);
  bool sent = false;
  if (two_way) {
    // 2W: prawdziwa komenda (z potwierdzeniem) + realny status za chwilę.
    const velux::VeluxRadio::TwoWayAction action =
        position == 0 ? velux::VeluxRadio::TwoWayAction::OPEN
                      : (position == 100 ? velux::VeluxRadio::TwoWayAction::CLOSE
                                         : velux::VeluxRadio::TwoWayAction::POSITION);
    radio_->queueTwoWay(slot_, action, position);
    sent = true;
  } else {
    sent = radio_ != nullptr && radio_->sendPosition(slot_, position);
  }
  if (!sent) {
    SUPLA_LOG_WARNING("VELUX[%d] nie udało się wysłać pozycji %u%%", channel.getChannelNumber(), position);
    return;
  }
  startAnimation_(position);
}

void VeluxCover::applyTwoWayStatus(const velux::VeluxRadio::TwoWayStatus &status) {
  if (!status.known) {
    return;
  }
  // setCurrentPosition() przyjmuje procenty (0 = otwarte, 100 = zamknięte),
  // skaluje je do jednostek 0,01 i czyści flagę "nie skalibrowano" - realny
  // odczyt z urządzenia jest lepszy niż szacunek.
  setCurrentPosition(status.position);

  if (status.stopped) {
    moving_ = false;
    move_duration_ms_ = 0;
    currentDirection = Supla::Control::Directions::STOP_DIR;
    targetPosition = static_cast<int8_t>(status.position);
    SUPLA_LOG_INFO("VELUX[%d] status 2W: %u%% (stoi)", channel.getChannelNumber(), status.position);
    return;
  }

  // Urządzenie jedzie: animujemy do znanego celu, a kolejny status zweryfikuje
  // wynik. Czas skalujemy odległością, bo to tylko szacunek dla aplikacji.
  int16_t target = currentPosition;
  if (targetPosition >= 0 && targetPosition <= 100) {
    target = static_cast<int16_t>(targetPosition) * 100;
  }
  const int32_t distance = target > currentPosition ? target - currentPosition : currentPosition - target;
  const bool closing = target > currentPosition;
  move_from_ = currentPosition;
  move_to_ = target;
  move_started_ms_ = millis();
  move_duration_ms_ = (closing ? closingTimeMs : openingTimeMs) * static_cast<uint32_t>(distance) / 10000;
  if (move_duration_ms_ == 0 || move_from_ == move_to_) {
    moving_ = false;
    currentDirection = Supla::Control::Directions::STOP_DIR;
  } else {
    moving_ = true;
    currentDirection = closing ? Supla::Control::Directions::DOWN_DIR : Supla::Control::Directions::UP_DIR;
  }
  SUPLA_LOG_INFO("VELUX[%d] status 2W: %u%% (w ruchu, cel %d%%)", channel.getChannelNumber(), status.position,
                 move_to_ / 100);
}

void VeluxCover::sendStop_() {
  bool sent = false;
  if (radio_ != nullptr && radio_->slotTwoWayReady(slot_)) {
    radio_->queueTwoWay(slot_, velux::VeluxRadio::TwoWayAction::STOP);
    sent = true;
  } else {
    sent = radio_ != nullptr && radio_->sendStop(slot_);
  }
  if (!sent) {
    SUPLA_LOG_WARNING("VELUX[%d] nie udało się wysłać STOP", channel.getChannelNumber());
  }
}

void VeluxCover::startAnimation_(uint8_t position) {
  const int16_t target = static_cast<int16_t>(position) * 100;
  const bool closing = target > currentPosition;

  move_from_ = currentPosition < 0 ? target : currentPosition;
  move_to_ = target;
  move_duration_ms_ = closing ? closingTimeMs : openingTimeMs;
  move_started_ms_ = millis();
  currentDirection = closing ? Supla::Control::Directions::DOWN_DIR : Supla::Control::Directions::UP_DIR;

  if (move_duration_ms_ == 0 || move_from_ == move_to_) {
    currentPosition = move_to_;
    moving_ = false;
    currentDirection = Supla::Control::Directions::STOP_DIR;
  } else {
    moving_ = true;
  }
}

void VeluxCover::updateAnimation_() {
  if (!moving_) {
    return;
  }
  const uint32_t elapsed = millis() - move_started_ms_;
  if (elapsed >= move_duration_ms_) {
    currentPosition = move_to_;
    moving_ = false;
    currentDirection = Supla::Control::Directions::STOP_DIR;
    return;
  }
  currentPosition =
      move_from_ + (static_cast<int32_t>(move_to_ - move_from_) * static_cast<int32_t>(elapsed)) /
                       static_cast<int32_t>(move_duration_ms_);
}
