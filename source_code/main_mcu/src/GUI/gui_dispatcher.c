/*!  \file     gui_dispatcher.c
*    \brief    GUI functions dispatcher
*    Created:  16/11/2018
*    Author:   Mathieu Stephan
*/
#include "comms_hid_msgs_debug.h"
#include "gui_dispatcher.h"
#include "gui_carousel.h"
#include "gui_prompts.h"
#include "gui_menu.h"
#include "defines.h"
#include "inputs.h"
#include "main.h"
// Current screen
gui_screen_te gui_dispatcher_current_screen = GUI_SCREEN_NINSERTED;


/*! \fn     gui_dispatcher_set_current_screen(gui_screen_te screen)
*   \brief  Set current screen
*   \param  screen          The screen
*   \param  reset_states    Set to true to reset previously selected items
*/
void gui_dispatcher_set_current_screen(gui_screen_te screen, BOOL reset_states, oled_transition_te transition)
{
    /* Store transition, screen, call menu reset routine */
    plat_oled_descriptor.loaded_transition = transition;
    gui_dispatcher_current_screen = screen;    
    gui_menu_reset_selected_items(reset_states);
    
    /* If we're going into a menu, set the selected menu */
    if ((screen >= GUI_SCREEN_MAIN_MENU) && (screen <= GUI_SCREEN_SETTINGS))
    {
        gui_menu_set_selected_menu(screen-GUI_SCREEN_MAIN_MENU+MAIN_MENU);
        gui_menu_update_menus();
    }
}

/*! \fn     gui_dispatcher_get_back_to_current_screen(void)
*   \brief  Get back to the current screen
*/
void gui_dispatcher_get_back_to_current_screen(void)
{
    /* switch to let the compiler optimize instead of function pointer array */
    switch (gui_dispatcher_current_screen)
    {
        case GUI_SCREEN_NINSERTED:          {
                                                sh1122_display_bitmap_from_flash_at_recommended_position(&plat_oled_descriptor, GUI_LOCKED_MINI_BITMAP_ID, TRUE); 
                                                #ifdef OLED_INTERNAL_FRAME_BUFFER
                                                    sh1122_flush_frame_buffer(&plat_oled_descriptor);
                                                #endif
                                                break;
                                            }                                                
        case GUI_SCREEN_INSERTED_LCK:       break;
        case GUI_SCREEN_INSERTED_INVALID:   gui_prompts_display_information_on_screen(ID_STRING_REMOVE_CARD); break;
        case GUI_SCREEN_INSERTED_UNKNOWN:   break;
        case GUI_SCREEN_MEMORY_MGMT:        break;
        case GUI_SCREEN_CATEGORIES:         break;
        case GUI_SCREEN_FAVORITES:          break;
        case GUI_SCREEN_LOGIN:              break;
        case GUI_SCREEN_LOCK:               break;
        /* Common menu architecture */
        case GUI_SCREEN_MAIN_MENU:
        case GUI_SCREEN_BT:
        case GUI_SCREEN_OPERATIONS:
        case GUI_SCREEN_SETTINGS:           gui_menu_event_render(WHEEL_ACTION_NONE);break;
        default: break;
    }
}

/*! \fn     gui_dispatcher_event_dispatch(wheel_action_ret_te wheel_action)
*   \brief  Called for the GUI to do something with a wheel action
*   \param  wheel_action    Wheel action that just happened
*/
void gui_dispatcher_event_dispatch(wheel_action_ret_te wheel_action)
{
    /* Bool to know if we should rerender */
    BOOL rerender_bool = FALSE;
    
    /* switch to let the compiler optimize instead of function pointer array */
    switch (gui_dispatcher_current_screen)
    {
        case GUI_SCREEN_NINSERTED:          break;
        case GUI_SCREEN_INSERTED_LCK:       break;
        case GUI_SCREEN_INSERTED_INVALID:   break;
        case GUI_SCREEN_INSERTED_UNKNOWN:   break;
        case GUI_SCREEN_MEMORY_MGMT:        break;
        case GUI_SCREEN_CATEGORIES:         break;
        case GUI_SCREEN_FAVORITES:          break;
        case GUI_SCREEN_LOGIN:              break;
        case GUI_SCREEN_LOCK:               break;
        /* Common menu architecture */        
        case GUI_SCREEN_MAIN_MENU:
        case GUI_SCREEN_BT:
        case GUI_SCREEN_OPERATIONS:
        case GUI_SCREEN_SETTINGS:           rerender_bool = gui_menu_event_render(wheel_action);break;
        default: break;
    }    
    
    /* Should we rerender? */
    if (rerender_bool != FALSE)
    {
        gui_dispatcher_get_back_to_current_screen();
    }
}

/*! \fn     gui_dispatcher_main_loop(void)
*   \brief  GUI main loop
*/
void gui_dispatcher_main_loop(void)
{
    // Get user action
    wheel_action_ret_te user_action = inputs_get_wheel_action(FALSE, FALSE);
    
    // Run main GUI screen loop if there was an action. TODO: screen saver
    if (user_action != WHEEL_ACTION_NONE)
    {
        gui_dispatcher_event_dispatch(user_action);
    }
}