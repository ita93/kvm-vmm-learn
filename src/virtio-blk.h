#pragma once

#include <linux/virtio_blk.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "diskimg.h"
#include "pci.h"
#include "virtio-pci.h"
#include "virtq.h"
