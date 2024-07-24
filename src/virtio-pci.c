#include <cstddef>
#include <fcntl.h>
#include <iterator>
#include <linux/virtio_config.h>
#include <linux/virtio_pci.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "pci.h"
#include "utils.h"
#include "virtio-pci.h"
#include "virtq.h"

static void virtio_pci_select_device_feature(struct virtio_pci_dev *dev) {
  uint32_t select = dev->config.common_cfg.device_feature_select;
  uint64_t feature = dev->device_feature;

  switch (select) {
  case 0:
    dev->config.common_cfg.device_feature = feature;
    break;
  case 1:
    dev->config.common_cfg.device_feature = feature >> 32;
    break;
  default:
    dev->config.common_cfg.device_feature = 0;
    break;
  }
}

static void virtio_pci_write_guest_feature(struct virtio_pci_dev *dev) {
  uint32_t select = dev->config.common_cfg.guest_feature_select;
  uint32_t feature = dev->config.common_cfg.guest_feature;

  switch (select) {
  case 0:
    dev->guest_feature |= feature;
    break;
  case 1:
    dev->guest_feature |= (uint64_t)feature << 32;
    break;
  default:
    break;
  }
}

static void virtio_pci_reset(struct virtio_pci_dev *dev) {
  // TODO: virtio pci reset
}

static void virtio_pci_write_status(struct virtio_pci_dev *dev) {
  uint8_t status = dev->config.common_cfg.device_status;
  if (status == 0) {
    virtio_pci_reset(dev);
  }
}

static void virtio_pci_selec_virtq(struct virtio_pci_dev *dev) {
  uint16_t select = dev->config.common_cfg.queue_select;
  struct virtio_pci_common_cfg *config = &dev->config.common_cfg;

  if (select < config->num_queues) {
    uint64_t offset = offsetof(struct virtio_pci_common_cfg, queue_size);
    memcpy((void *)config + offset, &dev->vq[select].info,
           sizeof(struct virtq_info));
  } else {
    config->queue_size = 0;
  }
}

static void virtio_pci_enable_virtq(struct virtio_pci_dev *dev) {
  uint16_t select = dev->config.common_cfg.queue_select;
  virtq_enable(&dev->vq[select]);
}

static void virtio_pci_disable_virtq(struct virtio_pci_dev *dev) {
  uint16_t select = dev->config.common_cfg.queue_select;
  virtq_disable(&dev->vq[select]);
}

static void virtio_pci_space_write(struct virtio_pci_dev *dev, void *data,
                                   uint64_t offset, uint8_t size) {
  if (offset < offsetof(struct virtio_pci_config, dev_cfg)) {
    memcpy((void *)&dev->config + offset, data, size);
    switch (offset) {
    case VIRTIO_PCI_COMMON_DFSELECT:
      virtio_pci_select_device_feature(dev);
      break;
    case VIRTIO_PCI_COMMON_GFSELECT:
      virtio_pci_write_guest_feature(dev);
      break;
    case VIRTIO_PCI_COMMON_STATUS:
      virtio_pci_write_status(dev);
      break;
    case VIRTIO_PCI_COMMON_Q_SELECT:
      virtio_pci_selec_virtq(dev);
      break;
    case VIRTIO_PCI_COMMON_Q_ENABLE:
      if (dev->config.common_cfg.queue_enable)
        virtio_pci_enable(dev);
      else
        virtio_pci_disable_virtq(dev);
      break;
    default:
      if (offset >= VIRTIO_PCI_COMMON_Q_SIZE &&
          offset <= VIRTIO_PCI_COMMON_Q_USEDHI) {
        uint16_t select = dev->config.common_cfg.queue_select;
        uint64_t info_offset = offset - VIRTIO_PCI_COMMON_Q_SIZE;
        if (select < dev->config.common_cfg.num_queues)
          memcpy((void *)&dev->vq[select].info + info_offset, data, size);
      } else if (offset == offsetof(struct virtio_pci_config, notify_data)) {
        virtq_handle_avail(&dev->vq[dev->config.notify_data.vqn]);
      }
      break;
    }
    return;
  }

  /*dev config write*/
  uint64_t dev_offset = offset - offsetof(struct virtio_pci_config, dev_cfg);
  memcpy((void *)dev->config.dev_cfg + dev_offset, data, size);
}

static void virtio_pci_space_read(struct virtio_pci_dev *dev,
                                  void *data,
                                  uint64_t offset,
                                  uint8_t size)
{
  if (offset < offsetof(struct virtio_pci_config, dev_cfg)) {
    memcpy(data,(void *)&dev->config + offset, size);
    if (offset == offsetof(struct virtio_pci_config, isr_cap)) {
      dev->config.isr_cap.isr_status = 0;
    }
  } else {
    /* dev config read */
    uint64_t dev_offset = 
      offset - offsetof(struct virtio_pci_config, dev_cfg);
    memcpy(data, (void*)dev->config.dev_cfg + dev_offset, size);
  }
}

static void virtio_pci_space_io(void *owner,
                                void *data,
                                uint8_t is_write,
                                uint64_t offset,
                                uint8_t size)
{
    struct virtio_pci_dev *virtio_pci_dev =
        container_of(owner, struct virtio_pci_dev, pci_dev);
    if (is_write)
        virtio_pci_space_write(virtio_pci_dev, data, offset, size);
    else
        virtio_pci_space_read(virtio_pci_dev, data, offset, size);
}

static void virtio_pci_set_cap(struct virtio_pci_dev *dev, uint8_t next)
{
  struct virtio_pci_cap *caps[VIRTIO_PCI_CAP_NUM + 1];

  for (int i = 1; i < VIRTIO_PCI_CAP_NUM + 1; i++)
  {
    caps[i] = dev->pci_dev.hdr + next;
    *caps[i] = (struct virtio_pci_cap) {
      .cap_vndr = PCI_CAP_ID_VNDR,
      .cfg_type = i,
      .cap_len = sizeof(struct virtio_pci_cap),
      .bar = 0,
    };
    if (i == VIRTIO_PCI_CAP_NOTIFY_CFG || i == VIRTIO_PCI_CAP_PCI_CFG)
      caps[i]->cap_len += sizeof(uint32_t);
    next += caps[i]->cap_len;
    caps[i]->cap_next = next;
  }

  caps[VIRTIO_PCI_CAP_COMMON_CFG]->offset = 
    offsetof(struct virtio_pci_config, common_cfg);
  caps[VIRTIO_PCI_CAP_COMMON_CFG]->length = 
    sizeof(struct virtio_pci_common_cfg);
}
