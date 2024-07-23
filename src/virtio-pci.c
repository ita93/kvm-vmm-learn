#include <fcntl.h>
#include <linux/virtio_config.h>
#include <linux/virtio_pci.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "pci.h"
#include "utils.h"
#include "virtq.h"
#include "virtio-pci.h"

static void virtio_pci_select_device_feature(struct virtio_pci_dev *dev)
{
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

static void virtio_pci_write_guest_feature(struct virtio_pci_dev *dev)
{
  uint32_t select = dev->config.common_cfg.guest_feature_select;
  uint32_t feature = dev->config.common_cfg.guest_feature;

  switch (select) {
    case 0:
      dev->guest_feature |= feature;
    break;
    case 1:
      dev->guest_feature |= (uint64_t) feature << 32;
    break;
    default:
    break;
  }
}

static void virtio_pci_reset(struct virtio_pci_dev *dev)
{
  // TODO: virtio pci reset
}

static void virtio_pci_write_status(struct virtio_pci_dev *dev)
{
  uint8_t status = dev->config.common_cfg.device_status;
  if (status == 0) {
    virtio_pci_reset(dev);
  }
}

static void virtio_pci_selec_virtq(struct virtio_pci_dev *dev)
{
  uint16_t select = dev->config.common_cfg.queue_select;
  struct virtio_pci_common_cfg *config = &dev->config.common_cfg;

  if (select< config->num_queues){
    uint64_t offset = offsetof(struct virtio_pci_common_cfg, queue_size);
    memcpy((void*)config + offset, &dev->vq[select].info,
           sizeof(struct virtq_info));
  } else {
    config->queue_size = 0;
  }
}

static void virtio_pci_enable_virtq(struct virtio_pci_dev *dev)
{
  uint16_t select = dev->config.common_cfg.queue_select;
  virtq_enable(&dev->vq[select]);
}

static void virtio_pci_disable_virtq(struct virtio_pci_dev *dev)
{
  uint16_t select = dev->config.common_cfg.queue_select;
  virtq_disable(&dev->vq[select]);
}
