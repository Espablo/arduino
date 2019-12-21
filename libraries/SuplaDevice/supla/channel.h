/*
 Copyright (C) AC SOFTWARE SP. Z O.O.

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; either version 2
 of the License, or (at your option) any later version.
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#ifndef _channel_h
#define _channel_h

#include "../supla-common/proto.h"
#include <Arduino.h>
#include "tools.h"

namespace Supla {

    /*
class Channel;

class ChannelIterator {
  public:
    ChannelIterator& operator++() {
      if (currentPtr) {
        currentPtr = currentPtr->nextPtr;
      }
      return *this;
    }

    ChannelIterator(Element *ptr) {
      currentPtr = ptr;
    }

  protected:
    Channel *currentPtr;
}
     */

class Channel {
  public:

    Channel() {
      valueChanged = false;
      channelNumber = -1;
      if (reg_dev.channel_count < SUPLA_CHANNELMAXCOUNT) {
        channelNumber = reg_dev.channel_count;
        reg_dev.channels[channelNumber].Number = channelNumber;
        reg_dev.channel_count++;
      } 

      if (firstPtr == nullptr) {
        firstPtr = this;
      } else {
        last()->nextPtr = this;
      }
      nextPtr = nullptr;
    }

    static Channel *begin() {
      return firstPtr;
    }

    static Channel *last() {
      Channel *ptr = firstPtr;
      while (ptr && ptr->nextPtr) {
        ptr = ptr->nextPtr;
      }
      return ptr;
    }

    static int size() {
      int count = 0;
      Channel *ptr = firstPtr;
      if (ptr) {
        count++;
      }
      while (ptr->nextPtr) {
        count++;
        ptr = ptr->nextPtr;
      }
      return count;

    }

    bool isUpdateReady() { return valueChanged; };
    void setNewValue(double dbl) {
      if (sizeof(double) == 8) {
        memcpy(&(reg_dev.channels[channelNumber].value), &dbl, 8);
      } else if (sizeof(double) == 4) {
        float2DoublePacked(dbl, (byte *)&(reg_dev.channels[channelNumber].value));
    }
      
    }
    virtual bool isExtended() {
      return false;
    }

    void setType(int type) {
      if (channelNumber >= 0) {
        reg_dev.channels[channelNumber].Type = type;
      }
    }

    void setDefault(int value) {
      if (channelNumber >= 0) {
        reg_dev.channels[channelNumber].Default = value;
      }
    }

    static TDS_SuplaRegisterDevice_D reg_dev;

  protected:
    void setUpdateReady() { valueChanged = true; };
    void clearUpdateReady() { valueChanged = false; };

    bool valueChanged;
    int channelNumber;

    Channel *nextPtr;
    static Channel *firstPtr;
};

};  // namespace Supla

#endif
