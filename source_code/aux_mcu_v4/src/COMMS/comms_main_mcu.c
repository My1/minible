/*!  \file     comms_aux_mcu.c
*    \brief    Communications with aux MCU
*    Created:  03/03/2018
*    Author:   Mathieu Stephan
*/
#include <string.h>
#include <asf.h>
#include "comms_hid_msgs_debug.h"
#include "platform_defines.h"
#include "logic_bluetooth.h"
#include "comms_hid_msgs.h"
#include "comms_main_mcu.h"
#include "logic_keyboard.h"
#include "comms_raw_hid.h"
#include "logic_battery.h"
#include "driver_timer.h"
#include "driver_timer.h"
#include "logic_sleep.h"
#include "ble_manager.h"
#include "ble_manager.h"
#include "platform_io.h"
#include "logic_sleep.h"
#include "at_ble_api.h"
#include "ble_utils.h"
#include "defines.h"
#include "debug.h"
#include "logic.h"
#include "main.h"
#include "dma.h"
#include "udc.h"
/* Message about to be sent to main MCU */
aux_mcu_message_t main_mcu_send_message;
/* Temporary message, used when dealing with message shorter than max size */
volatile aux_mcu_message_t comms_main_mcu_temp_message;
/* Flag set if we have treated a message by only looking at its first bytes */
volatile BOOL comms_main_mcu_usb_msg_answered_using_first_bytes = FALSE;
volatile BOOL comms_main_mcu_ble_msg_answered_using_first_bytes = FALSE;
volatile BOOL comms_main_mcu_other_msg_answered_using_first_bytes = FALSE;

/*! \fn     comms_main_init_rx(void)
*   \brief  Init communications with aux MCU
*/
void comms_main_init_rx(void)
{
    dma_main_mcu_init_rx_transfer();
}

/*! \fn     comms_main_mcu_get_temp_tx_message_object_pt(void)
*   \brief  Get a pointer to our temporary tx message object
*/
aux_mcu_message_t* comms_main_mcu_get_temp_tx_message_object_pt(void)
{
    return (aux_mcu_message_t*)&main_mcu_send_message;
}

/*! \fn     comms_main_mcu_get_temp_rx_message_object_pt(void)
*   \brief  Get a pointer to our temporary rx message object
*/
aux_mcu_message_t* comms_main_mcu_get_temp_rx_message_object_pt(void)
{
    return (aux_mcu_message_t*)&comms_main_mcu_temp_message;
}

/*! \fn     comms_main_mcu_get_empty_packet_ready_to_be_sent(aux_mcu_message_t** message_pt_pt, uint16_t message_type)
*   \brief  Get an empty message ready to be sent
*   \param  message_pt_pt           Pointer to where to store message pointer
*   \param  message_type            Message type
*/
void comms_main_mcu_get_empty_packet_ready_to_be_sent(aux_mcu_message_t** message_pt_pt, uint16_t message_type)
{
    /* Wait for possible ongoing message to be sent */
    dma_wait_for_main_mcu_packet_sent();
    
    /* Get pointer to our message to be sent buffer */
    aux_mcu_message_t* temp_tx_message_pt = &main_mcu_send_message;
    
    /* Clear it */
    memset((void*)temp_tx_message_pt, 0, sizeof(*temp_tx_message_pt));
    
    /* Populate the fields */
    temp_tx_message_pt->message_type = message_type;
    
    /* Store message pointer */
    *message_pt_pt = temp_tx_message_pt;
}

/*! \fn     comms_main_mcu_send_simple_event(uint16_t event_id)
*   \brief  Send a simple event to main MCU
*   \param  event_id    The event ID
*/
void comms_main_mcu_send_simple_event(uint16_t event_id)
{
    dma_wait_for_main_mcu_packet_sent();
    main_mcu_send_message.aux_mcu_event_message.event_id = event_id;
    main_mcu_send_message.message_type = AUX_MCU_MSG_TYPE_AUX_MCU_EVENT;
    main_mcu_send_message.payload_length1 = sizeof(main_mcu_send_message.aux_mcu_event_message.event_id);
    comms_main_mcu_send_message((void*)&main_mcu_send_message, (uint16_t)sizeof(aux_mcu_message_t));
}

/*! \fn     comms_main_mcu_send_message(aux_mcu_message_t* message, uint16_t message_length)
*   \brief  Send a message to the MCU
*   \param  message         Pointer to the message to send
*   \param  message_length  Message length
*   \note   Transfer is done through DMA so data will be accessed after this function returns
*/
void comms_main_mcu_send_message(aux_mcu_message_t* message, uint16_t message_length)
{
    /* Wake-up main MCU if it is currently sleeping */
    logic_sleep_wakeup_main_mcu_if_needed();
    
    /* The function below does wait for a previous transfer to finish and does check for no comms */
    dma_main_mcu_init_tx_transfer((void*)&AUXMCU_SERCOM->USART.DATA.reg, (void*)message, sizeof(aux_mcu_message_t));    
}

/*! \fn     comms_main_mcu_deal_with_non_usb_non_ble_message(void)
*   \brief  Routine dealing with non USB & non BLE messages
*   \param  message Message to deal with
*/
void comms_main_mcu_deal_with_non_usb_non_ble_message(aux_mcu_message_t* message)
{
    #ifdef MAIN_MCU_MSG_DBG_PRINT
    comms_usb_debug_printf("Received main MCU other message: %i, %i", message->message_type, message->payload_as_uint16[0]);
    #endif
    
    /* Special reboot packet: check for only 0xff */
    BOOL special_reboot_packet = TRUE;
    uint8_t* rcv_message_recast = (uint8_t*)message;
    for (uint16_t i = 0; i < sizeof(aux_mcu_message_t); i++)
    {
        if (rcv_message_recast[i] != 0xFF)
        {
            special_reboot_packet = FALSE;
            break;
        }
    }
    
    /* Reboot if needed */
    if (special_reboot_packet != FALSE)
    {
        cpu_irq_disable();
        NVIC_SystemReset();        
    }
    
    if (message->message_type == AUX_MCU_MSG_TYPE_PLAT_DETAILS)
    {
        /* Status request */
        memset((void*)&main_mcu_send_message, 0x00, sizeof(aux_mcu_message_t));
        main_mcu_send_message.message_type = message->message_type;
        main_mcu_send_message.payload_length1 = sizeof(aux_plat_details_message_t);
        main_mcu_send_message.aux_details_message.aux_fw_ver_major = FW_MAJOR;
        main_mcu_send_message.aux_details_message.aux_fw_ver_minor = FW_MINOR;
        main_mcu_send_message.aux_details_message.aux_did_register = DSU->DID.reg;
        main_mcu_send_message.aux_details_message.aux_uid_registers[0] = *(uint32_t*)0x0080A00C;
        main_mcu_send_message.aux_details_message.aux_uid_registers[1] = *(uint32_t*)0x0080A040;
        main_mcu_send_message.aux_details_message.aux_uid_registers[2] = *(uint32_t*)0x0080A044;
        main_mcu_send_message.aux_details_message.aux_uid_registers[3] = *(uint32_t*)0x0080A048;
        
        /* Check if BLE is enabled */
        if (logic_is_ble_enabled() != FALSE)
        {
            /* Blusdk lib version */
            main_mcu_send_message.aux_details_message.blusdk_lib_maj = BLE_SDK_MAJOR_NO(BLE_SDK_VERSION);
            main_mcu_send_message.aux_details_message.blusdk_lib_min = BLE_SDK_MINOR_NO(BLE_SDK_VERSION);
            
            /* Try to get fw version */
            uint32_t blusdk_fw_ver;
            if(at_ble_firmware_version_get(&blusdk_fw_ver) == AT_BLE_SUCCESS)
            {
                main_mcu_send_message.aux_details_message.blusdk_fw_maj = BLE_SDK_MAJOR_NO(blusdk_fw_ver);
                main_mcu_send_message.aux_details_message.blusdk_fw_min = BLE_SDK_MINOR_NO(blusdk_fw_ver);
                main_mcu_send_message.aux_details_message.blusdk_fw_build = BLE_SDK_BUILD_NO(blusdk_fw_ver);
                
                /* Getting RF version */
                at_ble_rf_version_get(&main_mcu_send_message.aux_details_message.atbtlc_rf_ver);
                
                /* ATBTLC1000 chip ID */
                at_ble_chip_id_get(&main_mcu_send_message.aux_details_message.atbtlc_chip_id);
                
                /* ATBTLC address */
                at_ble_addr_t atbtlc_address;
                atbtlc_address.type = AT_BLE_ADDRESS_PUBLIC;
                at_ble_addr_get(&atbtlc_address);
                memcpy((void*)main_mcu_send_message.aux_details_message.atbtlc_address, (void*)atbtlc_address.addr, sizeof(atbtlc_address.addr));
            }
        }
        
        /* Send message */
        comms_main_mcu_send_message((void*)&main_mcu_send_message, (uint16_t)sizeof(main_mcu_send_message));
    }
    else if (message->message_type == AUX_MCU_MSG_TYPE_NIMH_CHARGE)
    {
        /* Return charging status */
        memset((void*)&main_mcu_send_message, 0x00, sizeof(aux_mcu_message_t));
        main_mcu_send_message.message_type = message->message_type;
        main_mcu_send_message.payload_length1 = sizeof(nimh_charge_message_t);
        main_mcu_send_message.nimh_charge_message.charge_status = logic_battery_get_charging_status();
        main_mcu_send_message.nimh_charge_message.battery_voltage = logic_battery_get_vbat();
        main_mcu_send_message.nimh_charge_message.charge_current = logic_battery_get_charging_current();
        main_mcu_send_message.nimh_charge_message.dac_data_reg = platform_io_get_dac_data_register_set();
        main_mcu_send_message.nimh_charge_message.stepdown_voltage = logic_battery_get_stepdown_voltage();
        
        /* Send message */
        comms_main_mcu_send_message((void*)&main_mcu_send_message, (uint16_t)sizeof(main_mcu_send_message));
    }
    else if (message->message_type == AUX_MCU_MSG_TYPE_BOOTLOADER)
    {
        if (message->bootloader_message.command == BOOTLOADER_START_PROGRAMMING_COMMAND)
        {
            main_set_bootloader_flag();
            cpu_irq_disable();
            NVIC_SystemReset();
        }
    }
    else if (message->message_type == AUX_MCU_MSG_TYPE_PING_WITH_INFO)
    {
        message->message_type = AUX_MCU_MSG_TYPE_AUX_MCU_EVENT;
        message->aux_mcu_event_message.event_id = AUX_MCU_EVENT_IM_HERE;
        message->payload_length1 = sizeof(message->aux_mcu_event_message.event_id);
        comms_main_mcu_send_message((void*)message, (uint16_t)sizeof(aux_mcu_message_t));
    }    
    else if (message->message_type == AUX_MCU_MSG_TYPE_KEYBOARD_TYPE)
    {
        BOOL typing_success_bool = TRUE;
        
        /* Iterate over symbols */
        uint16_t counter = 0;
        while(message->keyboard_type_message.keyboard_symbols[counter] != 0)
        {
            uint16_t symbol = message->keyboard_type_message.keyboard_symbols[counter];
            
            if (symbol == 0xFFFF)
            {
                /* Original unicode point can't be typed */
            }            
            else if ((symbol & 0x7F00) == 0)
            {
                BOOL is_dead_key = FALSE;
                
                /* Check for dead key */
                if ((symbol & 0x8000) != 0)
                {
                    is_dead_key = TRUE;
                }                    
                
                /* One key to be typed */
                if (logic_keyboard_type_symbol((hid_interface_te)message->keyboard_type_message.interface_identifier, (uint8_t)symbol, is_dead_key, message->keyboard_type_message.delay_between_types) != RETURN_OK)
                {
                    typing_success_bool = FALSE;
                    break;
                }
            }
            else
            {
                /* Two keys to be typed */
                if (logic_keyboard_type_symbol((hid_interface_te)message->keyboard_type_message.interface_identifier, (uint8_t)(symbol >> 8), FALSE, message->keyboard_type_message.delay_between_types) != RETURN_OK)
                {
                    typing_success_bool = FALSE;
                    break;
                }
                if (logic_keyboard_type_symbol((hid_interface_te)message->keyboard_type_message.interface_identifier, (uint8_t)symbol, FALSE, message->keyboard_type_message.delay_between_types) != RETURN_OK)
                {
                    typing_success_bool = FALSE;
                    break;
                }
            }
            
            /* Move on to the next symbol */
            counter++;
        }
            
        /* Send success status */
        memset((void*)message, 0x00, sizeof(aux_mcu_message_t));
        message->message_type = AUX_MCU_MSG_TYPE_KEYBOARD_TYPE;
        message->payload_as_uint16[0] = (uint16_t)typing_success_bool;
        message->payload_length1 = sizeof(uint16_t);
        comms_main_mcu_send_message((void*)message, (uint16_t)sizeof(aux_mcu_message_t));
    }
    else if (message->message_type == AUX_MCU_MSG_TYPE_BLE_CMD)
    {
        switch(message->ble_message.message_id)
        {
            case BLE_MESSAGE_CMD_ENABLE:
            {
                /* Enable BLE */
                if (logic_is_ble_enabled() == FALSE)
                {
                    logic_bluetooth_start_bluetooth(message->ble_message.payload);
                    logic_set_ble_enabled();
                }
                message->message_type = AUX_MCU_MSG_TYPE_AUX_MCU_EVENT;
                message->aux_mcu_event_message.event_id = AUX_MCU_EVENT_BLE_ENABLED;
                message->payload_length1 = sizeof(message->aux_mcu_event_message.event_id);
                comms_main_mcu_send_message((void*)message, (uint16_t)sizeof(aux_mcu_message_t));
                break;
            }
            case BLE_MESSAGE_CMD_DISABLE:
            {
                /* Enable BLE */
                if (logic_is_ble_enabled() != FALSE)
                {
                    logic_bluetooth_stop_bluetooth();
                    logic_set_ble_disabled();
                }
                message->message_type = AUX_MCU_MSG_TYPE_AUX_MCU_EVENT;
                message->aux_mcu_event_message.event_id = AUX_MCU_EVENT_BLE_DISABLED;
                message->payload_length1 = sizeof(message->aux_mcu_event_message.event_id);
                comms_main_mcu_send_message((void*)message, (uint16_t)sizeof(aux_mcu_message_t));
                break;
            }
            default:
            {
                break;
            }
        }
    }
    else if (message->message_type == AUX_MCU_MSG_TYPE_MAIN_MCU_CMD)
    {
        switch(message->main_mcu_command_message.command)
        {
            case MAIN_MCU_COMMAND_SLEEP:
            {
                /* Wait for interrupt to clear this flag if set (wait for full packet receive) */
                while (comms_main_mcu_other_msg_answered_using_first_bytes != FALSE);   
                
                /* Send ACK */
                comms_main_mcu_send_simple_event(AUX_MCU_EVENT_SLEEP_RECEIVED);                 
                dma_wait_for_main_mcu_packet_sent();
                
                /* Different strategies depending on if BLE is enabled */
                if (logic_is_ble_enabled() == FALSE)
                {
                    /* Disable main comms */
                    platform_io_disable_main_comms();
                    
                    /* BLE disabled, go to sleep directly */
                    main_standby_sleep(FALSE);
                    
                    /* We're awake, re-init comms */
                    platform_io_enable_main_comms();
                    comms_main_init_rx();
                }
                else
                {
                    /* Disable main comms */
                    platform_io_disable_main_comms();
                    
                    /* Set flag */
                    logic_sleep_set_full_platform_sleep_requested();
                }                    
                           
                break;
            }
            case MAIN_MCU_COMMAND_PING:
            {
                /* Resend same message, not used anymore, obsolete */
                comms_main_mcu_send_message((void*)message, (uint16_t)sizeof(aux_mcu_message_t));
                break;
            }
            case MAIN_MCU_COMMAND_ATTACH_USB:
            {
                /* Start ADC conversions for when we're later asked to charge battery */
                logic_battery_start_using_adc();
                
                /* Attach USB resistors */
                udc_attach();
                break;
            }
            case MAIN_MCU_COMMAND_DETACH_USB:
            {
                /* Detach USB resistors */
                comms_usb_clear_enumerated();
                udc_detach();
                
                /* Stop charging if we are */
                logic_battery_stop_charging();
                
                /* Stop using the ADC */
                logic_battery_stop_using_adc();

                /* Inform main MCU */
                comms_main_mcu_send_simple_event(AUX_MCU_EVENT_USB_DETACHED);
                break;
            }
            case MAIN_MCU_COMMAND_NIMH_CHARGE:
            {
                /* Charge NiMH battery */
                logic_battery_start_charging(NIMH_23C_CHARGING);
                break;
            }
            case MAIN_MCU_COMMAND_STOP_CHARGE:
            {
                /* Stop charging battery */
                logic_battery_stop_charging();
                break;
            }
            case MAIN_MCU_COMMAND_SET_BATTERYLVL:
            {
                /* Set battery level on bluetooth service */
                logic_bluetooth_set_battery_level(message->main_mcu_command_message.payload[0]);
                break;
            }
            case MAIN_MCU_COMMAND_NO_COMMS_UNAV:
            {
                /* No comms signal unavailable */
                logic_set_nocomms_unavailable();
                break;
            }
            case MAIN_MCU_COMMAND_TX_SWEEP_SGL:
            {
                /* ble_manager will send the event to main MCU */
                debug_tx_band_send(message->main_mcu_command_message.payload_as_uint16[0], message->main_mcu_command_message.payload_as_uint16[1], message->main_mcu_command_message.payload_as_uint16[2], FALSE);
                break;
            }
            case MAIN_MCU_COMMAND_TX_TONE_CONT:
            {
                /* ble_manager will send the event to main MCU */
                debug_tx_band_send(message->main_mcu_command_message.payload_as_uint16[0], message->main_mcu_command_message.payload_as_uint16[1], message->main_mcu_command_message.payload_as_uint16[2], TRUE);
                break;
            }
            case MAIN_MCU_COMMAND_TX_TONE_CONT_STOP:
            {
                debug_tx_stop_continuous_tone();
                break;
            }
            case MAIN_MCU_COMMAND_FORCE_CHARGE_VOLT:
            {
                logic_battery_debug_force_charge_voltage(message->main_mcu_command_message.payload_as_uint16[0]);
                break;
            }
            case MAIN_MCU_COMMAND_STOP_FORCE_CHARGE:
            {
                logic_battery_debug_stop_charge();
                break;
            }
            case MAIN_MCU_COMMAND_FUNC_TEST:
            {
                /* Functional test: prepare answer message */
                message->aux_mcu_event_message.payload[0] = 0;
                message->message_type = AUX_MCU_MSG_TYPE_AUX_MCU_EVENT;
                message->aux_mcu_event_message.event_id = AUX_MCU_EVENT_FUNC_TEST_DONE;
                message->payload_length1 = sizeof(message->aux_mcu_event_message.event_id) + sizeof(uint8_t);
                
                /* Functional test: start by turning on bluetooth */
                uint8_t temp_mac_address[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x12, 0x13};
                logic_bluetooth_start_bluetooth(temp_mac_address);
                if (ble_sdk_version() == 0)
                {
                    message->aux_mcu_event_message.payload[0] = 1;
                    comms_main_mcu_send_message((void*)message, (uint16_t)sizeof(aux_mcu_message_t));
                    break;
                }
                
                /* Battery charging tests: set charging LDO to max voltage without enabling charge mosfets and make sure there's no charging current */
                platform_io_enable_step_down(1800);
                
                /* Discard first measurement */
                while (platform_io_is_current_sense_conversion_result_ready() == FALSE);
                platform_io_get_cursense_conversion_result(TRUE);
                
                /* Measure charging current */
                while (platform_io_is_current_sense_conversion_result_ready() == FALSE);
                uint32_t cur_sense_vs = platform_io_get_cursense_conversion_result(FALSE);
                volatile int16_t high_voltage = (int16_t)(cur_sense_vs >> 16);
                volatile int16_t low_voltage = (int16_t)cur_sense_vs;
                
                /* Charging current? (check for 40mA) */
                if ((high_voltage - low_voltage) > 100)
                {
                    platform_io_disable_step_down();
                    message->aux_mcu_event_message.payload[0] = 2;
                    comms_main_mcu_send_message((void*)message, (uint16_t)sizeof(aux_mcu_message_t));
                    break;                    
                }    
                
                /* Set voltage at a reasonable value */
                uint16_t current_charge_voltage = 1200;
                platform_io_get_cursense_conversion_result(TRUE);
                platform_io_update_step_down_voltage(current_charge_voltage);
                timer_delay_ms(5);
                
                /* Start charging! */
                platform_io_enable_charge_mosfets();
                
                /* Increase the voltage until we get some current (40mA) */
                while (TRUE)
                {
                    while (platform_io_is_current_sense_conversion_result_ready() == FALSE);
                    cur_sense_vs = platform_io_get_cursense_conversion_result(FALSE);
                    high_voltage = (int16_t)(cur_sense_vs >> 16);
                    low_voltage = (int16_t)cur_sense_vs;
                    
                    if ((high_voltage - low_voltage) > LOGIC_BATTERY_CUR_FOR_ST_RAMP_END)
                    {
                        /* All good! */
                        break;
                    }
                    else
                    {
                        /* Increase charge voltage */
                        current_charge_voltage += 10;
                        
                        /* Check for over voltage - may be caused by disconnected charge path */
                        if ((low_voltage >= LOGIC_BATTERY_MAX_V_FOR_ST_RAMP) || (current_charge_voltage > 1650))
                        {
                            message->aux_mcu_event_message.payload[0] = 2;
                            break;
                        }
                        else
                        {
                            /* Increment voltage */
                            platform_io_update_step_down_voltage(current_charge_voltage);
                            platform_io_get_cursense_conversion_result(TRUE);
                        }
                    }
                }
                
                /* Disable DC/DC and mosfets */
                platform_io_disable_charge_mosfets();
                timer_delay_ms(1);
                platform_io_disable_step_down();
                
                /* Send functional test result */
                comms_main_mcu_send_message((void*)message, (uint16_t)sizeof(aux_mcu_message_t));      
                break;          
            }
            case MAIN_MCU_COMMAND_UPDT_DEV_STAT:
            {
                /* Update device status buffer */
                comms_raw_hid_update_device_status_cache(message->main_mcu_command_message.payload);
                break;
            }
            default:
            {
                break;
            }
        }
    }
}

/*! \fn     comms_main_mcu_routine(BOOL filter_and_force_use_of_temp_receive_buffer, uint16_t expected_message_type)
*   \brief  Routine dealing with main mcu comms
*   \param  filter_and_force_use_of_temp_receive_buffer     Set to TRUE to force all received messages to be stored in the temp buffer and to not parse message if it matches expected_message_type
*   \param  expected_message_type                           If filter_and_force_use_of_temp_receive_buffer is set to TRUE, expected message type to return RETURN_OK (NOT AUX_MCU_MSG_TYPE_USB & AUX_MCU_MSG_TYPE_BLE)
*   \return RETURN_OK if filter_and_force_use_of_temp_receive_buffer is set to TRUE and received message type is of type expected_message_type, otherwise RETURN_NOK
*/
ret_type_te comms_main_mcu_routine(BOOL filter_and_force_use_of_temp_receive_buffer, uint16_t expected_message_type)
{	
    /* First: deal with fully received messages */
    if (dma_main_mcu_usb_msg_received != FALSE)
    {
        /* Set bool and do necessary action: no point in setting the bool after the function call as the dma receiver will overwrite the packet anyways */
        dma_main_mcu_usb_msg_received = FALSE;
        
        if (comms_main_mcu_usb_msg_answered_using_first_bytes == FALSE)
        {
            comms_raw_hid_send_hid_message(USB_INTERFACE, (aux_mcu_message_t*)&dma_main_mcu_usb_rcv_message);
        }
    }
    if (dma_main_mcu_ble_msg_received != FALSE)
    {
        /* Set bool and do necessary action: no point in setting the bool after the function call as the dma receiver will overwrite the packet anyways */
        dma_main_mcu_ble_msg_received = FALSE;
        
        if (comms_main_mcu_ble_msg_answered_using_first_bytes == FALSE)
        {
            comms_raw_hid_send_hid_message(BLE_INTERFACE, (aux_mcu_message_t*)&dma_main_mcu_ble_rcv_message);
        }
    }
    if (dma_main_mcu_other_msg_received != FALSE)
    {
        /* Set bool and do necessary action: no point in setting the bool after the function call as the dma receiver will overwrite the packet anyways */
        dma_main_mcu_other_msg_received = FALSE;
        
        if (comms_main_mcu_other_msg_answered_using_first_bytes == FALSE)
        {
            if ((filter_and_force_use_of_temp_receive_buffer != FALSE) && (dma_main_mcu_other_message.message_type == expected_message_type))
            {
                memcpy((void*)&comms_main_mcu_temp_message, (void*)&dma_main_mcu_other_message, sizeof(comms_main_mcu_temp_message));
                return RETURN_OK;
            } 
            else
            {
                comms_main_mcu_deal_with_non_usb_non_ble_message((aux_mcu_message_t*)&dma_main_mcu_other_message);
            }
        }
    }
    
    /* Second: see if we could deal with a packet in advance */
    /* Ongoing RX transfer received bytes */
    uint16_t nb_received_bytes_for_ongoing_transfer = sizeof(dma_main_mcu_temp_rcv_message) - dma_main_mcu_get_remaining_bytes_for_rx_transfer();
    
    /* Depending on the message type, set the correct bool pointers */
    volatile BOOL* answered_with_the_first_bytes_pointer = &comms_main_mcu_other_msg_answered_using_first_bytes;
    volatile BOOL* packet_fully_received_in_the_mean_time_pointer = &dma_main_mcu_other_msg_received;
    if (dma_main_mcu_temp_rcv_message.message_type == AUX_MCU_MSG_TYPE_USB)
    {
        answered_with_the_first_bytes_pointer = &comms_main_mcu_usb_msg_answered_using_first_bytes;
        packet_fully_received_in_the_mean_time_pointer = &dma_main_mcu_usb_msg_received;
    } 
    else if (dma_main_mcu_temp_rcv_message.message_type == AUX_MCU_MSG_TYPE_BLE)
    {
        answered_with_the_first_bytes_pointer = &comms_main_mcu_ble_msg_answered_using_first_bytes;
        packet_fully_received_in_the_mean_time_pointer = &dma_main_mcu_ble_msg_received;
    }
    
    /* Check if we should deal with this packet */    
    cpu_irq_enter_critical();
    BOOL should_deal_with_packet = FALSE;
    
    /* Conditions: received more bytes than the payload length, didn't already reply using this method, received flag didn't arrive in the mean time */
    if ((nb_received_bytes_for_ongoing_transfer >= sizeof(dma_main_mcu_temp_rcv_message.message_type) + sizeof(dma_main_mcu_temp_rcv_message.payload_length1) + dma_main_mcu_temp_rcv_message.payload_length1) && (*answered_with_the_first_bytes_pointer == FALSE) && ((sizeof(aux_mcu_message_t) - nb_received_bytes_for_ongoing_transfer) > 20) && (*packet_fully_received_in_the_mean_time_pointer == FALSE))
    {
        should_deal_with_packet = TRUE;
        
        /* Set bool and do necessary action: no point in setting the bool after the function call as the dma receiver will overwrite the packet anyways */
        *answered_with_the_first_bytes_pointer = TRUE;
        
        /* Copy message into dedicated buffer, as the message currently is in the dma temporary buffer. */
        memset((void*)&comms_main_mcu_temp_message, 0, sizeof(comms_main_mcu_temp_message));
        memcpy((void*)&comms_main_mcu_temp_message, (void*)&dma_main_mcu_temp_rcv_message, nb_received_bytes_for_ongoing_transfer);
    }
    cpu_irq_leave_critical();
    
    /* First part of message */
    if (should_deal_with_packet != FALSE)
    {        
        if (comms_main_mcu_temp_message.message_type == AUX_MCU_MSG_TYPE_USB)
        {
            comms_raw_hid_send_hid_message(USB_INTERFACE, (aux_mcu_message_t*)&comms_main_mcu_temp_message);
        }
        else if (comms_main_mcu_temp_message.message_type == AUX_MCU_MSG_TYPE_BLE)
        {
            comms_raw_hid_send_hid_message(BLE_INTERFACE, (aux_mcu_message_t*)&comms_main_mcu_temp_message);
        }
        else
        {
            if ((filter_and_force_use_of_temp_receive_buffer != FALSE) && (comms_main_mcu_temp_message.message_type == expected_message_type))
            {
                return RETURN_OK;
            }
            else
            {
                comms_main_mcu_deal_with_non_usb_non_ble_message((aux_mcu_message_t*)&comms_main_mcu_temp_message);  
            }                
        }
    }
    
    return RETURN_NOK;
}

/*! \fn     comms_main_mcu_get_32_rng_bytes_from_main_mcu(uint8_t* buffer)
*   \brief  Get 32 random bytes from main MCU
*   \param  buffer  Where to store the random bytes
*/
void comms_main_mcu_get_32_rng_bytes_from_main_mcu(uint8_t* buffer)
{
    aux_mcu_message_t* temp_rx_message_pt = comms_main_mcu_get_temp_rx_message_object_pt();
    aux_mcu_message_t* temp_tx_message_pt;
    
    /* Generate our packet */
    comms_main_mcu_get_empty_packet_ready_to_be_sent(&temp_tx_message_pt, AUX_MCU_MSG_TYPE_RNG_TRANSFER);   
    
    /* Set random payload size, as it doesn't matter */
    temp_tx_message_pt->payload_length1 = 1;
    
    /* Send packet */
    comms_main_mcu_send_message((void*)temp_tx_message_pt, (uint16_t)sizeof(aux_mcu_message_t));
    
    /* Wait for message from main MCU */
    while (comms_main_mcu_routine(TRUE, AUX_MCU_MSG_TYPE_RNG_TRANSFER) != RETURN_OK);
    
    /* Received message is in temporary buffer */
    memcpy((void*)buffer, (void*)temp_rx_message_pt->payload_as_uint16, 32);
}

/*! \fn     comms_main_mcu_fetch_bonding_info_for_mac(uint8_t address_resolv_type, uint8_t* mac_addr, nodemgmt_bluetooth_bonding_information_t* bonding_info)
*   \brief  Try to fetch bonding information for a given MAC
*   \param  address_resolv_type Type of address
*   \param  mac_addr            The MAC address
*   \param  bonding_infoWhere to store the bonding info if we find it
*   \return if we managed to find bonding info
*/
ret_type_te comms_main_mcu_fetch_bonding_info_for_mac(uint8_t address_resolv_type, uint8_t* mac_addr, nodemgmt_bluetooth_bonding_information_t* bonding_info)
{
    aux_mcu_message_t* temp_rx_message_pt = comms_main_mcu_get_temp_rx_message_object_pt();
    aux_mcu_message_t* temp_tx_message_pt;
    
    /* Generate our packet */
    comms_main_mcu_get_empty_packet_ready_to_be_sent(&temp_tx_message_pt, AUX_MCU_MSG_TYPE_BLE_CMD);
    temp_tx_message_pt->ble_message.message_id = BLE_MESSAGE_RECALL_BOND_INFO;
    temp_tx_message_pt->ble_message.payload[0] = address_resolv_type;
    memcpy(&temp_tx_message_pt->ble_message.payload[1], mac_addr, 6);
    
    /* Set payload size */
    temp_tx_message_pt->payload_length1 = sizeof(address_resolv_type) + sizeof(temp_tx_message_pt->ble_message.message_id) + 6;
    
    /* Send packet */
    comms_main_mcu_send_message((void*)temp_tx_message_pt, (uint16_t)sizeof(aux_mcu_message_t));
    
    /* Wait for message from main MCU */
    while (comms_main_mcu_routine(TRUE, AUX_MCU_MSG_TYPE_BLE_CMD) != RETURN_OK);
    
    /* Check for success and do necessary actions */
    if (temp_rx_message_pt->payload_length1 == sizeof(temp_tx_message_pt->ble_message.message_id) + sizeof(nodemgmt_bluetooth_bonding_information_t))
    {
        memcpy(bonding_info, &temp_rx_message_pt->ble_message.bonding_information_to_store_message, sizeof(nodemgmt_bluetooth_bonding_information_t));
        return RETURN_OK;
    } 
    else
    {
        return RETURN_NOK;
    }
}