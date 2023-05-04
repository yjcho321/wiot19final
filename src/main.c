#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/kernel.h>

#include <zephyr/settings/settings.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/bluetooth/services/hrs.h>
#include <zephyr/bluetooth/services/ias.h>

#include <nfc_t2t_lib.h>
#include <nfc/ndef/msg.h>
#include <nfc/ndef/text_rec.h>

#include <dk_buttons_and_leds.h>

static int update_nfc_data();

static struct bt_conn *default_conn;
static uint16_t default_conn_handle;
static struct k_work adv_work;
static bool is_advertising;

#define ALARM_RSSI_THRESHOLD -70

#define BT_STATUS_LED		DK_LED1
#define AUTH_LED			DK_LED2
#define ALARM_ARMED_LED		DK_LED3
#define ALARM_LED			DK_LED4

#define WIOT19_SERVICE_UUID BT_UUID_128_ENCODE(0xBDFC9792, 0x8234, 0x405E, 0xAE02, 0x35EF3274B299)

#define MAX_PASSWORD_LEN 20
static uint8_t password[MAX_PASSWORD_LEN + 1] = "hello";
static bool is_authenticated;

static bool is_armed, is_alarm_on;
static uint8_t data_update_blink_counter;

static int8_t rssi;
static bool receiving_rssi, receiving_armed;

#define MAX_FIELD_LEN 20
static uint8_t pet_name[MAX_FIELD_LEN + 1] = "pet name";
static uint8_t owner_name[MAX_FIELD_LEN + 1] = "owner name";
static uint8_t owner_address[MAX_FIELD_LEN + 1] = "owner address";
static uint8_t owner_phone[MAX_FIELD_LEN + 1] = "owner phone";

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, WIOT19_SERVICE_UUID),
};
static ssize_t read_test(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			void *buf, uint16_t len, uint16_t offset)
{
	uint8_t test[] = "testing: ";
	return bt_gatt_attr_read(conn, attr, buf, len, offset, test, strlen(test));
}


static ssize_t read_general(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			void *buf, uint16_t len, uint16_t offset)
{
	const uint8_t *value = attr->user_data;
	return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
}
static ssize_t write_auth(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 const void *buf, uint16_t len, uint16_t offset,
			 uint8_t flags)
{
	uint8_t *value = attr->user_data;
	uint8_t buffer[MAX_PASSWORD_LEN + 1];
	if (offset + len > MAX_PASSWORD_LEN) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	memcpy(buffer, buf, len);
	buffer[len] = 0;
	if (strcmp(password, buffer) != 0) {
		printk("authentication fail\n");
		*value = false;
		return BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION);
	}
	printk("authentication pass\n");
	*value = true;

	return 1;
}
static ssize_t write_general_protected(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 const void *buf, uint16_t len, uint16_t offset,
			 uint8_t flags)
{
	bool *value = attr->user_data;

	if (len < 1) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (!is_authenticated) {
		return BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION);
	}

	*value = ((uint8_t*)buf)[0];

	return 1;
}

static ssize_t read_string(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			void *buf, uint16_t len, uint16_t offset)
{
	const char *value = attr->user_data;
	return bt_gatt_attr_read(conn, attr, buf, len, offset, value, strlen(value));
}
static ssize_t write_string_protected(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 const void *buf, uint16_t len, uint16_t offset,
			 uint8_t flags)
{
	char *value = attr->user_data;

	if (offset + len > MAX_FIELD_LEN) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	if (!is_authenticated) {
		return BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION);
	}

	memcpy(value + offset, buf, len);
	value[offset + len] = 0;
	update_nfc_data();
	data_update_blink_counter = 4;

	return len;
}

static void rssi_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	receiving_rssi = value == BT_GATT_CCC_NOTIFY;
	printk("receiving rssi : %d\n", receiving_rssi);
}
static void armed_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	receiving_armed = value == BT_GATT_CCC_NOTIFY;
	printk("receiving armed : %d\n", receiving_armed);
}

BT_GATT_SERVICE_DEFINE(wiot19_service,
	BT_GATT_PRIMARY_SERVICE(
		BT_UUID_DECLARE_128(WIOT19_SERVICE_UUID)
	),
	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_16(0x0001), BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, read_general, NULL, &rssi), // rssi notify
	BT_GATT_CCC(rssi_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_16(0x0002), BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE, read_general, write_general_protected, &is_armed), // rssi notify
	BT_GATT_CCC(armed_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_16(0x0003), BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE, read_general, write_auth, &is_authenticated), // authentication
	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_16(0x0004), BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE, read_string, write_string_protected, pet_name), // pet name
	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_16(0x0005), BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE, read_string, write_string_protected, owner_name), // owner name
	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_16(0x0006), BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE, read_string, write_string_protected, owner_address), // owner address
	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_16(0x0007), BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE, read_string, write_string_protected, owner_phone), // owner phone
);

static void notify_rssi() {
	if(default_conn && receiving_rssi) {
		bt_gatt_notify(default_conn, &wiot19_service.attrs[2], &rssi, sizeof(rssi));		
		printk("Reported RSSI - %d\n", rssi);
	}
}

static void notify_armed() {
	if(default_conn && receiving_armed) {
		bt_gatt_notify(default_conn, &wiot19_service.attrs[5], &is_armed, sizeof(is_armed));		
		printk("Reported Alarm Armed - %d\n", is_armed);
	}
}

static void read_conn_rssi(uint16_t handle, int8_t *rssi)
{
	struct net_buf *buf, *rsp = NULL;
	struct bt_hci_cp_read_rssi *cp;
	struct bt_hci_rp_read_rssi *rp;

	int err;

	buf = bt_hci_cmd_create(BT_HCI_OP_READ_RSSI, sizeof(*cp));
	if (!buf) {
		printk("Unable to allocate command buffer\n");
		return;
	}

	cp = net_buf_add(buf, sizeof(*cp));
	cp->handle = sys_cpu_to_le16(handle);

	err = bt_hci_cmd_send_sync(BT_HCI_OP_READ_RSSI, buf, &rsp);
	if (err) {
		uint8_t reason = rsp ?
			((struct bt_hci_rp_read_rssi *)rsp->data)->status : 0;
		printk("Read RSSI err: %d reason 0x%02x\n", err, reason);
		return;
	}

	rp = (void *)rsp->data;
	*rssi = rp->rssi;

	net_buf_unref(rsp);
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	int ret;
	is_authenticated = false;
	is_armed = false;
	if (err) {
		printk("Connection failed (err 0x%02x)\n", err);
	} else {

		is_advertising = false;
		default_conn = bt_conn_ref(conn);
		ret = bt_hci_get_conn_handle(default_conn,
					     &default_conn_handle);
		err = bt_le_adv_stop();
		if (err) {
			printk("Advertising stop failed (err 0x%02x)\n", err);
		}
		printk("Connected\n");
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	is_authenticated = false;
	if (is_armed) {
		is_alarm_on = true;
	}	
	is_armed = false;
	if (default_conn) {
		bt_conn_unref(default_conn);
		default_conn = NULL;
	}
	printk("Disconnected (reason 0x%02x)\n", reason);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

static void bt_ready(void)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_NAME, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err) {
		printk("Advertising failed to start (err %d)\n", err);
		return;
	}
	is_advertising = true;
	printk("Advertising successfully started\n");
}

static void adv_handler(struct k_work *work)
{
	bt_ready();
}

#define MAX_REC_COUNT		4
#define NDEF_MSG_BUF_SIZE	256

static uint8_t ndef_msg_buf[NDEF_MSG_BUF_SIZE];

static const uint8_t en_code[] = {'e', 'n'};

#define MAX_PREFIX_LEN 15
static const uint8_t pet_name_prefix[] = "Pet Name: ";
static const uint8_t owner_name_prefix[] = "Owner Name: ";
static const uint8_t owner_address_prefix[] = "Owner Address: ";
static const uint8_t owner_phone_prefix[] = "Owner Phone: ";

static void concat_string_in_buffer(uint8_t *buffer, uint8_t *prefix, uint8_t *data) {
	memcpy(buffer, prefix, strlen(prefix));
	memcpy(buffer + strlen(prefix), data, strlen(data));
	buffer[strlen(prefix) + strlen(data)] = 0;
}

static int welcome_msg_encode(uint8_t *buffer, uint32_t *len)
{
	int err;
	uint8_t str_buf1[MAX_PREFIX_LEN + MAX_FIELD_LEN + 1];
	uint8_t str_buf2[MAX_PREFIX_LEN + MAX_FIELD_LEN + 1];
	uint8_t str_buf3[MAX_PREFIX_LEN + MAX_FIELD_LEN + 1];
	uint8_t str_buf4[MAX_PREFIX_LEN + MAX_FIELD_LEN + 1];

	concat_string_in_buffer(str_buf1, pet_name_prefix, pet_name);
	/* Create NFC NDEF text record description in English */
	NFC_NDEF_TEXT_RECORD_DESC_DEF(nfc_pet_name_text_rec,
				      UTF_8,
				      en_code,
				      sizeof(en_code),
				      str_buf1,
				      strlen(str_buf1));

	concat_string_in_buffer(str_buf2, owner_name_prefix, owner_name);
	NFC_NDEF_TEXT_RECORD_DESC_DEF(nfc_owner_name_text_rec,
				      UTF_8,
				      en_code,
				      sizeof(en_code),
				      str_buf2,
				      strlen(str_buf2));
					  
	concat_string_in_buffer(str_buf3, owner_address_prefix, owner_address);
	NFC_NDEF_TEXT_RECORD_DESC_DEF(nfc_owner_address_text_rec,
				      UTF_8,
				      en_code,
				      sizeof(en_code),
				      str_buf3,
				      strlen(str_buf3));
					  
	concat_string_in_buffer(str_buf4, owner_phone_prefix, owner_phone);
	NFC_NDEF_TEXT_RECORD_DESC_DEF(nfc_owner_phone_text_rec,
				      UTF_8,
				      en_code,
				      sizeof(en_code),
				      str_buf4,
				      strlen(str_buf4));

	NFC_NDEF_MSG_DEF(nfc_text_msg, MAX_REC_COUNT);

	err = nfc_ndef_msg_record_add(&NFC_NDEF_MSG(nfc_text_msg),
				   &NFC_NDEF_TEXT_RECORD_DESC(nfc_pet_name_text_rec));
	if (err < 0) {
		printk("Cannot add first record!\n");
		return err;
	}
	err = nfc_ndef_msg_record_add(&NFC_NDEF_MSG(nfc_text_msg),
				   &NFC_NDEF_TEXT_RECORD_DESC(nfc_owner_name_text_rec));
	if (err < 0) {
		printk("Cannot add second record!\n");
		return err;
	}

	err = nfc_ndef_msg_record_add(&NFC_NDEF_MSG(nfc_text_msg),
				   &NFC_NDEF_TEXT_RECORD_DESC(nfc_owner_address_text_rec));
	if (err < 0) {
		printk("Cannot add second record!\n");
		return err;
	}

	err = nfc_ndef_msg_record_add(&NFC_NDEF_MSG(nfc_text_msg),
				   &NFC_NDEF_TEXT_RECORD_DESC(nfc_owner_phone_text_rec));
	if (err < 0) {
		printk("Cannot add second record!\n");
		return err;
	}

	err = nfc_ndef_msg_encode(&NFC_NDEF_MSG(nfc_text_msg),
				      buffer,
				      len);
	if (err < 0) {
		printk("Cannot encode message!\n");
	}

	return err;
}

static int update_nfc_data() {
	uint32_t len = sizeof(ndef_msg_buf);
	int err;

	err = welcome_msg_encode(ndef_msg_buf, &len);
	if(!err) {
		err = nfc_t2t_payload_set(ndef_msg_buf, len);
	}
	return err;
}

static void nfc_callback(void *context, nfc_t2t_event_t event,
			 const uint8_t *data, size_t data_length)
{
	ARG_UNUSED(context);
	ARG_UNUSED(data);
	ARG_UNUSED(data_length);

	switch (event) {
	case NFC_T2T_EVENT_FIELD_ON:
		// dk_set_led_on(NFC_FIELD_LED);
		break;
	case NFC_T2T_EVENT_FIELD_OFF:
		// dk_set_led_off(NFC_FIELD_LED);
		break;
	case NFC_T2T_EVENT_DATA_READ:
		if (!default_conn && !is_advertising) {
			k_work_submit(&adv_work);
		}
		break;
	default:
		break;
	}
}

#define CLEAR_ALL_BT_MASK 		DK_BTN1_MSK
#define CLEAR_AUTH_MASK		 	DK_BTN2_MSK
#define SET_ARMED_MASK			DK_BTN3_MSK
#define CLEAR_ALARM_MASK 		DK_BTN4_MSK

void button_changed(uint32_t button_state, uint32_t has_changed)
{
	int err;
	uint32_t buttons = button_state & has_changed;

	if (buttons & CLEAR_ALL_BT_MASK) {
		if (default_conn) {
			err = bt_conn_disconnect(default_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
			if (err) {
				printk("Disconnect failed err: %d\n", err);
			}
		} else if (is_advertising) {
			err = bt_le_adv_stop();
			if (err) {
				printk("Advertising stop failed err: %d\n", err);
			} else {
				is_advertising = false;
			}
		}
	}
	if (buttons & CLEAR_AUTH_MASK) {
		is_authenticated = false;
	}

	if (buttons & SET_ARMED_MASK) {
		if(default_conn) {
			is_armed = !is_armed;
			notify_armed();
		}
	}

	if (buttons & CLEAR_ALARM_MASK) {
		is_armed = false;
		is_alarm_on = false;
		if(default_conn) {
			notify_armed();
		}
	}
}

static struct k_thread led_thread_data;
static K_THREAD_STACK_DEFINE(led_thread_stack, 256);

void manage_led(void *p1, void *p2, void *p3) {
	uint8_t ct = 0;
	while (1) {
		if (default_conn) {
			dk_set_led_on(BT_STATUS_LED);
		} else if (is_advertising) {
			dk_set_led(BT_STATUS_LED, ct % 2);
		} else {
			dk_set_led_off(BT_STATUS_LED);
		}

		if (default_conn && is_authenticated) {
			if (!data_update_blink_counter) {
				dk_set_led_on(AUTH_LED);
			} else {
				dk_set_led(AUTH_LED, data_update_blink_counter-- % 2);
			}
		} else {
			dk_set_led_off(AUTH_LED);
		}

		if (default_conn && is_armed) {
			dk_set_led_on(ALARM_ARMED_LED);
		} else {
			dk_set_led_off(ALARM_ARMED_LED);
		}

		if (is_alarm_on) {
			dk_set_led(ALARM_LED, ct % 2);
		} else {
			dk_set_led_off(ALARM_LED);
		}

		ct++;
		k_sleep(K_MSEC(250));
	}
}


void main(void)
{
	int err;

	err = dk_leds_init();
	if (err) {
		printk("Cannot init LEDs!\n");
		return;
	}

	err = dk_buttons_init(button_changed);
	if (err) {
		printk("Cannot init buttons (err %d\n", err);
		return;
	}

	err = nfc_t2t_setup(nfc_callback, NULL);
	if (err) {
		printk("Cannot setup NFC T2T library!\n");
		return;
	}

	err = update_nfc_data();
	if (err) {
		printk("Failed to update NFC data");
		return;
	}

	err = nfc_t2t_emulation_start();
	if (err) {
		printk("Cannot start emulation!\n");
		return;
	}
	printk("NFC configuration done\n");
	
	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return;
	}
	printk("Bluetooth init done\n");

	k_work_init(&adv_work, adv_handler);

	k_thread_create(&led_thread_data, led_thread_stack,
			K_THREAD_STACK_SIZEOF(led_thread_stack),
			manage_led, NULL, NULL, NULL,
			K_PRIO_COOP(10),
			0, K_NO_WAIT);

	uint8_t low_rssi_count = 0;
	while (1) {
		k_sleep(K_SECONDS(1));

		if (default_conn) {
			int8_t temp = 0xFF;
			read_conn_rssi(default_conn_handle, &temp);
			if(temp != 0xFF) {
				rssi = temp;
				notify_rssi();

				if(is_armed) {
					if (rssi > ALARM_RSSI_THRESHOLD) {
						is_alarm_on = false;
						low_rssi_count = 0;
					} else {
						low_rssi_count++;
					}

					if (low_rssi_count > 5) {
						is_alarm_on = true;
					}
				}
			}
		}
	}
}
