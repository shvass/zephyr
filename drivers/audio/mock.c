
#include <zephyr/audio/codec.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>

#define DT_DRV_COMPAT zephyr_mock_audio_codec

LOG_MODULE_REGISTER(mock_audio_codec, LOG_LEVEL_INF);

struct mock_codec_config {
	const struct device *uart_dev;
};

struct mock_codec_data {
	audio_codec_rx_done_callback_t user_rx_cb;
	audio_codec_tx_done_callback_t user_tx_cb;
	void *user_data_rx, *user_data_tx;

	uint8_t *rx_buf, *tx_buf;
	size_t buf_size;

	struct ring_buf tx_ring_buf;
};

static int mock_codec_write(const struct device *dev, const void *data, size_t size)
{
	struct mock_codec_data *dt = dev->data;
	const struct mock_codec_config *cfg = dev->config;
	struct ring_buf *tx_buffer = &dt->tx_ring_buf;
	uint8_t *raw_buf = 0;
	uint32_t avail = ring_buf_put_claim(tx_buffer, &raw_buf, size);

	if (avail == 0) {
		// @TODO no space in buffer, drop data or handle overflow
		LOG_WRN("No space in TX buffer, dropping data");
		return 0;
	}

	memcpy(raw_buf, data, avail);
	ring_buf_put_finish(tx_buffer, avail);

	uart_irq_tx_enable(cfg->uart_dev);

	return avail;
};

void mock_codec_uart_cb(const struct device *transport_dev, void *user_data)
{
	uart_irq_update(transport_dev);
	struct device *codec = user_data;
	struct mock_codec_data *data = codec->data;

	if (uart_irq_rx_ready(transport_dev)) {
		int read = uart_fifo_read(transport_dev, data->rx_buf, data->buf_size);
		if (read > 0 && data->user_rx_cb) {
			data->user_rx_cb(codec, data->rx_buf, read, data->user_data_rx);
		}
	};

	if (uart_irq_tx_ready(transport_dev)) {
		uint8_t *buf;
		struct ring_buf *tx_buffer = &data->tx_ring_buf;
		uint32_t avail = ring_buf_get_claim(tx_buffer, &buf, UINT32_MAX);

		if (avail == 0) {
			uart_irq_tx_disable(transport_dev);
			return;
		}

		int sent = uart_fifo_fill(transport_dev, buf, avail);

		if (sent > 0) {
			ring_buf_get_finish(tx_buffer, sent);

			if (data->user_tx_cb) {
				data->user_tx_cb(codec, data->user_data_tx);
			}
		}
	}
};

static int mock_codec_start(const struct device *dev)
{
	const struct mock_codec_config *mock_cfg = (struct mock_codec_config *)dev->config;
	struct device *transport = mock_cfg->uart_dev;

	uart_irq_rx_enable(transport);

	return 0;
}

static int mock_codec_stop(const struct device *dev)
{
	const struct mock_codec_config *mock_cfg = (struct mock_codec_config *)dev->config;
	struct device *transport = mock_cfg->uart_dev;

	// @todo handle callback set failure error
	uart_irq_rx_disable(transport);
	uart_irq_tx_disable(transport);
	return 0;
}

int mock_register_done_cb(const struct device *dev, audio_codec_tx_done_callback_t tx_cb,
			  void *tx_cb_user_data, audio_codec_rx_done_callback_t rx_cb,
			  void *rx_cb_user_data)
{
	struct mock_codec_data *data = dev->data;
	data->user_rx_cb = rx_cb;
	data->user_tx_cb = tx_cb;
	data->user_data_rx = rx_cb_user_data;
	data->user_data_tx = tx_cb_user_data;

	return 0;
};

static int mock_codec_configure(const struct device *dev, struct audio_codec_cfg *cfg)
{
	struct mock_codec_data *data = (struct mock_codec_data *)dev->data;
	data->buf_size = cfg->dai_cfg.pcm.block_size;

	// @todo replace k_malloc or handle memory properly
	data->rx_buf = k_malloc(data->buf_size);
	data->tx_buf = k_malloc(data->buf_size);

	ring_buf_init(&data->tx_ring_buf, data->buf_size, data->tx_buf);

	const struct mock_codec_config *mock_cfg = (struct mock_codec_config *)dev->config;
	struct device *transport = mock_cfg->uart_dev;

	// uart specific initialization,
	// register callback and user data
	int ret = uart_irq_callback_user_data_set(transport, mock_codec_uart_cb, dev);

	return ret;
}

static const struct audio_codec_api mock_codec_api = {.configure = mock_codec_configure,
						      .start = mock_codec_start,
						      .stop = mock_codec_stop,
						      .write = mock_codec_write,
						      .register_done_callback =
							      mock_register_done_cb};

static int mock_codec_init(const struct device *dev)
{
	const struct mock_codec_config *cfg = dev->config;

	if (!device_is_ready(cfg->uart_dev)) {
		return -ENODEV;
	}

	return 0;
}

#define MOCK_CODEC_INIT(inst)                                                                      \
	static struct mock_codec_data mock_codec_data_##inst;                                      \
                                                                                                   \
	static const struct mock_codec_config mock_codec_cfg_##inst = {                            \
		.uart_dev = DEVICE_DT_GET(DT_INST_PROP(inst, transport)),                          \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, mock_codec_init, NULL, &mock_codec_data_##inst,                \
			      &mock_codec_cfg_##inst, POST_KERNEL,                                 \
			      CONFIG_AUDIO_CODEC_INIT_PRIORITY, &mock_codec_api);

DT_INST_FOREACH_STATUS_OKAY(MOCK_CODEC_INIT)
