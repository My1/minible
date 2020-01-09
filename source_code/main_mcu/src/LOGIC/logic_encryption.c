/* 
 * This file is part of the Mooltipass Project (https://github.com/mooltipass).
 * Copyright (c) 2019 Stephan Mathieu
 * 
 * This program is free software: you can redistribute it and/or modify  
 * it under the terms of the GNU General Public License as published by  
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but 
 * WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU 
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
/*!  \file     logic_encryption.c
*    \brief    Encryption related functions, calling low level stuff
*    Created:  20/03/2019
*    Author:   Mathieu Stephan
*/
#include <string.h>
#include "logic_encryption.h"
#include "bearssl_block.h"
#include "bearssl_hash.h"
#include "bearssl_hmac.h"
#include "bearssl_rand.h"
#include "bearssl_ec.h"
#include "custom_fs.h"
#include "nodemgmt.h"
#include "rng.h"

// Next CTR value for our AES encryption
uint8_t logic_encryption_next_ctr_val[MEMBER_SIZE(nodemgmt_profile_main_data_t, current_ctr)];
// Current encryption context */
br_aes_ct_ctrcbc_keys logic_encryption_cur_aes_context;
// Current user CPZ user entry
cpz_lut_entry_t* logic_encryption_cur_cpz_entry;

static br_sha256_context logic_encryption_sha256_ctx;                       //Context used by the SHA256 engine for FIDO2
static br_sha512_context logic_encryption_sha512_ctx;                       //Same but for SHA512 for FIDO2
static br_hmac_key_context logic_encryption_hmac_kc;                        //Key context used by the HMAC engine for FIDO2
static br_hmac_context logic_encryption_hmac_ctx;                           //Context for the HMAC engine for FIDO2
static br_ec_impl const *logic_encryption_br_ec_algo = &br_ec_p256_m15;     //Selected algortihm that we use for FIDO2
static int logic_encryption_br_ec_algo_id = BR_EC_secp256r1;                //Selected subalgorithm in use for FIDO2
static br_hmac_drbg_context logic_encryption_hmac_drbg_ctx;                 //Context for the HMAC DRBG engine

static br_ec_private_key logic_encryption_fido2_signing_key;                      //Private signing key for signing during FIDO2 operation
                                                                            //This key is set every time a signing operation is performed and cleard afterwards
static uint8_t logic_encryption_fido2_priv_key_buf[FIDO2_PRIV_KEY_LEN];           //Private key buffer. Above has a pointer to this buffer

/*! \fn     logic_encryption_get_cur_cpz_lut_entry(void)
*   \brief  Get current user CPZ entry
*   \return Pointer to the entry or 0 if user isn't logged in
*/
cpz_lut_entry_t* logic_encryption_get_cur_cpz_lut_entry(void)
{
    if (logic_encryption_cur_cpz_entry != 0)
    {
        return logic_encryption_cur_cpz_entry;
    }
    else
    {
        return 0;
    }    
}

/*! \fn     logic_encryption_ctr_array_to_uint32(uint8_t* array)
*   \brief  Convert CTR array to uint32_t
*   \param  array   CTR array
*   \return The uint32_t
*   \note   The reason why the CTR is stored as array is backward compatibility
*/
static inline uint32_t logic_encryption_ctr_array_to_uint32(uint8_t* array)
{
    return (((uint32_t)array[2]) << 0) | (((uint32_t)array[1]) << 8) | (((uint32_t)array[0]) << 16);
}

/*! \fn     logic_encryption_add_vector_to_other(uint8_t* destination, uint8_t* source, uint16_t vector_length)
*   \brief  Add two vectors together
*   \param  destination     Array, which will also contain the result
*   \param  source          The other array
*   \param  vector_length   Vectors length
*   \note   MSB is at [0]
*/
void logic_encryption_add_vector_to_other(uint8_t* destination, uint8_t* source, uint16_t vector_length)
{
    uint16_t carry = 0;
    
    for (int16_t i = vector_length-1; i >= 0; i--)
    {
        carry = ((uint16_t)destination[i]) + ((uint16_t)source[i]) + carry;
        destination[i] = (uint8_t)(carry);
        carry = (carry >> 8) & 0x00FF;
    }    
}

/*! \fn     logic_encryption_xor_vector_to_other(uint8_t* destination, uint8_t* source, uint16_t vector_length)
*   \brief  XOR two vectors together
*   \param  destination     Array, which will also contain the result
*   \param  source          The other array
*   \param  vector_length   Vectors length
*/
void logic_encryption_xor_vector_to_other(uint8_t* destination, uint8_t* source, uint16_t vector_length)
{    
    for (int16_t i = vector_length-1; i >= 0; i--)
    {
        destination[i] = destination[i] ^ source[i];
    }    
}

/*! \fn     logic_encryption_get_cpz_ctr_entry(uint8_t* buffer)
*   \brief  Write the current user CPZ CTR Nonce entry in buffer
*   \param  buffer  Where to store the CPZ CTR Nonce entry
*/
void logic_encryption_get_cpz_ctr_entry(uint8_t* buffer)
{
    if (logic_encryption_cur_cpz_entry != 0)
    {
        memcpy(buffer, logic_encryption_cur_cpz_entry->cards_cpz, sizeof(logic_encryption_cur_cpz_entry->cards_cpz));
        memcpy(&(buffer[sizeof(logic_encryption_cur_cpz_entry->cards_cpz)]), logic_encryption_cur_cpz_entry->nonce, sizeof(logic_encryption_cur_cpz_entry->nonce));
    }
}

/*! \fn     logic_encryption_init_context(uint8_t* card_aes_key, cpz_lut_entry_t* cpz_user_entry)
*   \brief  Init encryption context for current user
*   \param  card_aes_key    AES key stored on user card
*   \param  cpz_user_entry  Pointer to memory persistent cpz entry
*/
void logic_encryption_init_context(uint8_t* card_aes_key, cpz_lut_entry_t* cpz_user_entry)
{
    /* Store CPZ user entry */
    logic_encryption_cur_cpz_entry = cpz_user_entry;
    
    /* Is this a fleet managed user account ? */
    if (logic_encryption_cur_cpz_entry->use_provisioned_key_flag == CUSTOM_FS_PROV_KEY_FLAG)
    {
        uint8_t user_provisioned_key[AES_KEY_LENGTH/8];
        uint8_t temp_ctr[AES256_CTR_LENGTH/8];
        
        /* Store provisioned key in our buffer */
        memcpy(user_provisioned_key, logic_encryption_cur_cpz_entry->provisioned_key, sizeof(user_provisioned_key));
        
        /* Set IV to 0 */
        memset(temp_ctr, 0, sizeof(temp_ctr));
        
        /* Use card AES key to decrypt flash-stored AES key */
        br_aes_ct_ctrcbc_init(&logic_encryption_cur_aes_context, card_aes_key, AES_KEY_LENGTH/8);        
        br_aes_ct_ctrcbc_ctr(&logic_encryption_cur_aes_context, (void*)temp_ctr, (void*)user_provisioned_key, sizeof(user_provisioned_key));
        
        /* Initialize encryption context */
        br_aes_ct_ctrcbc_init(&logic_encryption_cur_aes_context, user_provisioned_key, AES_KEY_LENGTH/8);
        nodemgmt_read_profile_ctr((void*)logic_encryption_next_ctr_val);
        
        /* Clear temp var */
        memset(user_provisioned_key, 0, sizeof(user_provisioned_key));
    } 
    else
    {
        /* Default user account: use smartcard AES key */
        br_aes_ct_ctrcbc_init(&logic_encryption_cur_aes_context, card_aes_key, AES_KEY_LENGTH/8);
        nodemgmt_read_profile_ctr((void*)logic_encryption_next_ctr_val);
    }
    /* Initialize ecc256 crypto engine. Uses RNG to initialize seed */
    logic_encryption_ecc256_init();
}

/*! \fn     logic_encryption_delete_context(void)
*   \brief  Delete encryption context
*/
void logic_encryption_delete_context(void)
{
    memset((void*)&logic_encryption_cur_aes_context, 0, sizeof(logic_encryption_cur_aes_context));
    logic_encryption_cur_cpz_entry = 0;
}

/*! \fn     logic_encryption_pre_ctr_tasks(void)
*   \brief  CTR pre encryption tasks
*   \param  ctr_inc     By how much we are planning to increment ctr value
*/
void logic_encryption_pre_ctr_tasks(uint16_t ctr_inc)
{
    uint8_t temp_buffer[MEMBER_SIZE(nodemgmt_profile_main_data_t, current_ctr)];
    uint16_t carry = CTR_FLASH_MIN_INCR;
    int16_t i;
    
    // Read CTR stored in flash
    nodemgmt_read_profile_ctr(temp_buffer);
    
    /* Check if the planned increment goes over the next val in the user profile */
    if (logic_encryption_ctr_array_to_uint32(logic_encryption_next_ctr_val) + ctr_inc >= logic_encryption_ctr_array_to_uint32(temp_buffer))
    {
        for (i = sizeof(temp_buffer)-1; i >= 0; i--)
        {
            carry = ((uint16_t)temp_buffer[i]) + carry;
            temp_buffer[i] = (uint8_t)(carry);
            carry = (carry >> 8) & 0x00FF;
        }
        nodemgmt_set_profile_ctr(temp_buffer);
    }    
}

/*! \fn     logic_encryption_post_ctr_tasks(uint16_t ctr_inc)
*   \brief  CTR post encryption tasks
*   \param  ctr_inc     By how much we should increment ctr value
*/
void logic_encryption_post_ctr_tasks(uint16_t ctr_inc)
{
    for (int16_t i = sizeof(logic_encryption_next_ctr_val)-1; i >= 0; i--)
    {
        ctr_inc = ((uint16_t)logic_encryption_next_ctr_val[i]) + ctr_inc;
        logic_encryption_next_ctr_val[i] = (uint8_t)(ctr_inc);
        ctr_inc = (ctr_inc >> 8) & 0x00FF;
    }    
}

/*! \fn     logic_encryption_ctr_encrypt(uint8_t* data, uint16_t data_length, uint8_t* ctr_val_used)
*   \brief  Encrypt data using next available CTR value
*   \param  data            Pointer to data
*   \param  data_length     Data length
*   \param  ctr_val_used    Where to store the CTR value used
*/
void logic_encryption_ctr_encrypt(uint8_t* data, uint16_t data_length, uint8_t* ctr_val_used)
{
        uint8_t credential_ctr[AES256_CTR_LENGTH/8];
        
        /* Pre CTR encryption tasks */
        logic_encryption_pre_ctr_tasks((data_length + AES256_CTR_LENGTH - 1)/AES256_CTR_LENGTH);
        
        /* Copy CTR value used for that credential */
        memcpy(ctr_val_used, logic_encryption_next_ctr_val, sizeof(logic_encryption_next_ctr_val));
        
        /* Construct CTR for this encryption */
        memcpy(credential_ctr, logic_encryption_cur_cpz_entry->nonce, sizeof(credential_ctr));
        logic_encryption_add_vector_to_other(credential_ctr + (sizeof(credential_ctr) - sizeof(logic_encryption_next_ctr_val)), logic_encryption_next_ctr_val, sizeof(logic_encryption_next_ctr_val));
        
        /* Encrypt data */        
        br_aes_ct_ctrcbc_ctr(&logic_encryption_cur_aes_context, (void*)credential_ctr, (void*)data, data_length);
        
        /* Reset vars */
        memset(credential_ctr, 0, sizeof(credential_ctr));
        
        /* Post CTR encryption tasks */
       logic_encryption_post_ctr_tasks((data_length + AES256_CTR_LENGTH - 1)/AES256_CTR_LENGTH);    
}

/*! \fn     logic_encryption_ctr_decrypt(uint8_t* data, uint8_t* cred_ctr, uint16_t data_length, BOOL old_gen_decrypt)
*   \brief  Decrypt data using provided ctr value
*   \param  data                Pointer to data
*   \param  cred_ctr            Credential CTR
*   \param  data_length         Data length
*   \param  old_gen_decrypt     Set to TRUE when decrypting original mini password
*/
void logic_encryption_ctr_decrypt(uint8_t* data, uint8_t* cred_ctr, uint16_t data_length, BOOL old_gen_decrypt)
{
    uint8_t credential_ctr[AES256_CTR_LENGTH/8];
    
    /* Construct CTR for this encryption */
    memcpy(credential_ctr, logic_encryption_cur_cpz_entry->nonce, sizeof(credential_ctr));
    if (old_gen_decrypt == FALSE)
    {
        logic_encryption_add_vector_to_other(credential_ctr + (sizeof(credential_ctr) - sizeof(logic_encryption_next_ctr_val)), cred_ctr, sizeof(logic_encryption_next_ctr_val));
    } 
    else
    {
        logic_encryption_xor_vector_to_other(credential_ctr + (sizeof(credential_ctr) - sizeof(logic_encryption_next_ctr_val)), cred_ctr, sizeof(logic_encryption_next_ctr_val));
    }
    
    /* Decrypt data */
    br_aes_ct_ctrcbc_ctr(&logic_encryption_cur_aes_context, (void*)credential_ctr, (void*)data, data_length);
    
    /* Reset vars */
    memset(credential_ctr, 0, sizeof(credential_ctr));  
}


/*! \fn     logic_encryption_sha256_init(void)
*   \brief  Initialize sha256 object
*/
void logic_encryption_sha256_init(void)
{
    br_sha256_init(&logic_encryption_sha256_ctx);
}

/*! \fn     logic_encryption_sha512_init(void)
*   \brief  Initialize sha512 object
*/
void logic_encryption_sha512_init(void)
{
    br_sha512_init(&logic_encryption_sha512_ctx);
}

/*! \fn     logic_encryption_sha256_update(uint8_t const * data, size_t len)
*   \brief  Update the sha256 value with the input data
*   \param  data    buffer containing the data to compute sha256 over
*   \param  len     length of data buffer
*/
void logic_encryption_sha256_update(uint8_t const * data, size_t len)
{
    br_sha256_update(&logic_encryption_sha256_ctx, data, len);
}

/*! \fn     logic_encryption_sha512_update(uint8_t const *data, size_t len)
*   \brief  Update the sha512 value with the input data
*   \param  data    buffer containing the data to compute sha512 over
*   \param  len     length of data buffer
*/
void logic_encryption_sha512_update(uint8_t const *data, size_t len)
{
    br_sha512_update(&logic_encryption_sha512_ctx, data, len);
}

/*! \fn     logic_encryption_sha256_final(uint8_t *hash)
*   \brief  Finalize the hash and out put the value to the argument
*   \param  hash  The final hash value (output)
*/
void logic_encryption_sha256_final(uint8_t *hash)
{
    br_sha256_out(&logic_encryption_sha256_ctx, hash);
}

/*! \fn     logic_encryption_sha512_final(uint8_t *hash)
*   \brief  Finalize the hash and output the value to the argument
*   \param  hash  The final hash value (output)
*/
void logic_encryption_sha512_final(uint8_t *hash)
{
    br_sha512_out(&logic_encryption_sha512_ctx, hash);
}

/*! \fn     logic_encryption_sha256_hmac_init(uint8_t const *key, uint32_t klen)
*   \brief  Initialize the sha256 HMAC object
*   \param  key     key used for HMAC
*   \param  len     length of key
*/
void logic_encryption_sha256_hmac_init(uint8_t const *key, uint32_t klen)
{
    br_hmac_key_init(&logic_encryption_hmac_kc, &br_sha256_vtable, key, klen);
    br_hmac_init(&logic_encryption_hmac_ctx, &logic_encryption_hmac_kc, 0);
}

/*! \fn     logic_encryption_sha256_hmac_update(uint8_t const *data, size_t len)
*   \brief  Update the HMAC256 value with the input data
*   \param  data    buffer containing the data to compute HMAC256 over
*   \param  len     length of data buffer
*/
void logic_encryption_sha256_hmac_update(uint8_t const *data, size_t len)
{
    br_hmac_update(&logic_encryption_hmac_ctx, data, len);
}

/*! \fn     logic_encryption_sha256_hmac_final(uint8_t * hmac_out)
*   \brief  Finalize the HMAC and output the value to the argument
*   \param  hash  The final HMAC value (output)
*/
void logic_encryption_sha256_hmac_final(uint8_t * hmac_out)
{
    br_hmac_out(&logic_encryption_hmac_ctx, hmac_out);
}

/*! \fn     logic_encryption_ecc256_init(void)
*   \brief  Initialize the ECC256 crypto object
*/
void logic_encryption_ecc256_init(void)
{
    uint8_t seed[ECC256_SEED_LENGTH];

    logic_encryption_br_ec_algo = &br_ec_p256_m15;
    logic_encryption_br_ec_algo_id = BR_EC_secp256r1;

    rng_fill_array(seed, ECC256_SEED_LENGTH);
    br_hmac_drbg_init(&logic_encryption_hmac_drbg_ctx, &br_sha256_vtable, seed, ECC256_SEED_LENGTH);
}

/*! \fn     logic_encryption_ecc256_sign(uint8_t const *data, int len, uint8_t * sig)
*   \brief  Cryptographically sign input data and return the signature in arg.
*   \param  data    data to sign
*   \param  len     length of data to sign
*   \param  sig     output signature
*/
void logic_encryption_ecc256_sign(uint8_t const *data, int len, uint8_t * sig)
{
    size_t result = br_ecdsa_i15_sign_raw(logic_encryption_br_ec_algo, logic_encryption_sha256_ctx.vtable, data, &logic_encryption_fido2_signing_key, sig);

    if (result == 0)
    {
        //TODO: Log(TAG_ERR, "error, signing failed!\n");
        while(1);
    }
    //Clear private key used to limit leaking
    memset(logic_encryption_fido2_priv_key_buf, 0, sizeof(logic_encryption_fido2_priv_key_buf));
}

/*! \fn     logic_encryption_create_fido2_priv_signing_key(uint8_t const *priv_key, br_ec_private_key *br_key)
*   \brief  Create key compatible with BearSSL library
*   \param  priv_key    Buffer containing the private key
*   \param  br_key      Output BearSSL compatible key
*/
static void logic_encryption_create_fido2_priv_signing_key(uint8_t const *priv_key, br_ec_private_key *br_key)
{
    memcpy(logic_encryption_fido2_priv_key_buf, priv_key, sizeof(logic_encryption_fido2_priv_key_buf));

    br_key->curve = logic_encryption_br_ec_algo_id;
    br_key->xlen = FIDO2_PRIV_KEY_LEN;
    br_key->x = logic_encryption_fido2_priv_key_buf;
}

/*! \fn     logic_encryption_ecc256_load_key(uint8_t const *key)
*   \brief  Load a key from a buffer
*   \param  key     The key to load
*/
void logic_encryption_ecc256_load_key(uint8_t const *key)
{
    logic_encryption_create_fido2_priv_signing_key(key, &logic_encryption_fido2_signing_key);
}

/*! \fn     logic_encryption_ecc256_generate_private_key(uint8_t *priv_key)
*   \brief  Generate a private key using ECC256
*   \param  priv_key    Output private key
*   \return size of key
*/
size_t logic_encryption_ecc256_generate_private_key(uint8_t *priv_key)
{
    size_t result;

    result = br_ec_keygen(&logic_encryption_hmac_drbg_ctx.vtable, logic_encryption_br_ec_algo, NULL, priv_key, logic_encryption_br_ec_algo_id);
    if (result == 0)
    {
        //Log("error, generating private key failed\n");
        while(1);
    }
    return result;
}

/*! \fn     logic_encryption_ecc256_derive_public_key(uint8_t const *priv_key, ecc256_pub_key *pub_key)
*   \brief  Derive public key from the private key
*   \param  priv_key    Private key to derive from
*   \param  pub_key     Output public key
*/
void logic_encryption_ecc256_derive_public_key(uint8_t const *priv_key, ecc256_pub_key *pub_key)
{
    #define PUBLIC_KEY_LEN 65

    uint8_t pubkey[PUBLIC_KEY_LEN];
    size_t result;
    br_ec_private_key br_priv_key;

    logic_encryption_create_fido2_priv_signing_key(priv_key, &br_priv_key);

    result = br_ec_compute_pub(logic_encryption_br_ec_algo, NULL, pubkey, &br_priv_key);
    if (result == 0)
    {
        //Log("error, deriving public key failed\n");
        while(1);
    }
    memmove(pub_key->x, pubkey + 1, FIDO2_PUB_KEY_X_LEN);
    memmove(pub_key->y, pubkey + 1 + FIDO2_PUB_KEY_X_LEN, FIDO2_PUB_KEY_Y_LEN);

    //Clear private key used to limit leaking
    memset(logic_encryption_fido2_priv_key_buf, 0, sizeof(logic_encryption_fido2_priv_key_buf));
}


